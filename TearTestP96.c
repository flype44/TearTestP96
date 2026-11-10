/*
    TearTestP96 - shows screen tearing on a Picasso96 screen, and what waiting for the vertical blank costs.

    Each frame is composed in fast memory, then copied to a 32-bit screen with p96WritePixelArray(), the way a
    game ported with SDL or MiniGL shows its frames. The picture is vertical colour bars scrolling sideways with
    a white bar bouncing over them: if the screen is scanned while a frame is copied, the bars break in two.

    Before each copy the program waits for:
      WaitBOVP  the vertical blank of the graphics card (its own video output)
      WaitTOF   the vertical blank of the Amiga chipset (another video output, another timing)
      NoWait    nothing: as many frames as the CPU can make

    Top right: frames a second, and the CPU time left to the other tasks ("CPU free"), counted by a task at the
    lowest priority. A driver with a vertical blank interrupt lets WaitBOVP sleep; without it the wait is a busy
    loop and CPU free drops to 0%.

    Keys: [SPACE] next wait mode, [ESC] quit.
    Template: MODE/K,SECONDS/N,WIDTH/N,HEIGHT/N
      MODE      WaitBOVP (default), WaitTOF or NoWait
      SECONDS   quit after that many seconds and print the averages (for scripted runs)
      WIDTH     size of the screen, 640x480 by default; the nearest 32-bit mode is taken
      HEIGHT

    Build: bebbo m68k-amigaos-gcc, libnix (see Makefile). Needs Picasso96API.library 2 or later.
*/

#include <exec/types.h>
#include <exec/memory.h>
#include <exec/tasks.h>
#include <dos/dos.h>
#include <devices/timer.h>
#include <graphics/gfx.h>
#include <graphics/text.h>
#include <graphics/rastport.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <libraries/Picasso96.h>

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/graphics.h>
#include <proto/intuition.h>
#include <proto/timer.h>
#include <proto/utility.h>
#include <proto/Picasso96.h>

#include <string.h>

static const char version[] __attribute__((used)) = "$VER: TearTestP96 1.0 (10.11.2026)";

/* ------------------------------------------------------------------------------------------------------------ */
/* Settings                                                                                                     */
/* ------------------------------------------------------------------------------------------------------------ */

#define DEFAULT_WIDTH   640
#define DEFAULT_HEIGHT  480

#define TEX_PERIOD      512         /* the bars repeat every 512 pixels (a power of 2) */
#define BAR_WIDTH       32          /* so 16 bars of colour */
#define SCROLL_SPEED    8           /* pixels a frame */
#define WHITE_WIDTH     24          /* the bouncing white bar */
#define WHITE_SPEED     14

#define TOP_PANEL       56          /* lines of the darkened band with the title and the meters */
#define BOTTOM_PANEL    32          /* lines of the darkened band with the keys */
#define PANEL_LINES     (TOP_PANEL + BOTTOM_PANEL)

#define IDLE_STACK      1024
#define CALIBRATE_TICKS 25          /* half a second: how fast the idle task counts on an idle machine */

#define RAWKEY_SPACE    0x40
#define RAWKEY_ESC      0x45

enum { MODE_WAITTOF, MODE_WAITBOVP, MODE_NOWAIT, MODE_COUNT };

static const char *const mode_name[MODE_COUNT] = { "WaitTOF", "WaitBOVP", "NoWait" };

/* Colours of the text; INK_NONE marks the pixels without text in the mask */
enum { INK_NONE, INK_SHADOW, INK_WHITE, INK_GREY, INK_YELLOW, INK_GREEN, INK_COUNT };

/* ------------------------------------------------------------------------------------------------------------ */
/* State                                                                                                        */
/* ------------------------------------------------------------------------------------------------------------ */

struct Library *P96Base;
struct Device *TimerBase;

/* The picture */
static ULONG width, height;
static ULONG pixfmt;                /* RGBFB_xxx of the screen, one of the four 32-bit formats */
static ULONG *tex;                  /* one period of bars, TEX_PERIOD pixels */
static ULONG *row;                  /* a line of the picture: every line between the panels is the same */
static ULONG *darkrow;              /* the same line at half brightness, the background of the panels */
static ULONG *textrow;              /* a line of a panel with its text */
static ULONG *frame;                /* the whole next frame, width x height */
static ULONG ink[INK_COUNT];

/* The text, drawn into a mask only when it changes (twice a second); for each panel line, the span of columns
   that has text, first > last when there is none */
static UBYTE glyph[96][8];          /* topaz 8, characters 32 to 127 */
static UBYTE *mask;                 /* PANEL_LINES x width, one ink a pixel */
static UWORD span_first[PANEL_LINES], span_last[PANEL_LINES];

/* The idle task, which counts while no other task runs */
static volatile ULONG idle_count;
static volatile BOOL idle_stop;
static struct Task *idle_parent;
static BYTE idle_sig = -1;

/* Everything main() opens, closed by Cleanup() */
struct App
{
    struct RDArgs *rdargs;
    struct timerequest timer;
    BOOL timer_open;
    struct Screen *screen;
    struct Window *window;
    UWORD *blank_pointer;
    APTR idle_mem;
    BOOL idle_running;
    struct Task *me;
    BYTE old_pri;
};

/* The measures */
struct Meter
{
    ULONG freq;                     /* EClock ticks a second */
    ULONG idle_rate;                /* counts a second of the idle task on an idle machine */
    struct EClockVal start, last;   /* start of the run, last update of the display */
    ULONG frames;                   /* frames since the last update */
    ULONG idle_at_last;             /* idle_count at the last update */
    ULONG total_frames;
    unsigned long long compose_ticks, copy_ticks, run_ticks;
    unsigned long long free_sum, free_n;
    char fps[16], cpu_free[20];     /* the strings on the screen */
};

/* ------------------------------------------------------------------------------------------------------------ */
/* Pixels                                                                                                       */
/* ------------------------------------------------------------------------------------------------------------ */

/* A pixel in the format of the screen */
static ULONG Pack(ULONG r, ULONG g, ULONG b)
{
    switch (pixfmt)
    {
        case RGBFB_B8G8R8A8: return (b << 24) | (g << 16) | (r << 8);
        case RGBFB_R8G8B8A8: return (r << 24) | (g << 16) | (b << 8);
        case RGBFB_A8B8G8R8: return (b << 16) | (g << 8) | r;
        default:             return (r << 16) | (g << 8) | b;       /* RGBFB_A8R8G8B8 */
    }
}

/* Half the brightness, whatever the order of the bytes */
static inline ULONG Dark(ULONG p)
{
    return (p >> 1) & 0x7f7f7f7f;
}

/* A hue (0-255 around the colour wheel) at full saturation */
static void Hue(ULONG h, ULONG *r, ULONG *g, ULONG *b)
{
    ULONG sector = (h * 6) >> 8;
    ULONG f = (h * 6) & 255;

    switch (sector)
    {
        case 0:  *r = 255;     *g = f;       *b = 0;       break;
        case 1:  *r = 255 - f; *g = 255;     *b = 0;       break;
        case 2:  *r = 0;       *g = 255;     *b = f;       break;
        case 3:  *r = 0;       *g = 255 - f; *b = 255;     break;
        case 4:  *r = f;       *g = 0;       *b = 255;     break;
        default: *r = 255;     *g = 0;       *b = 255 - f; break;
    }
}

/* The bars (one hue each, shaded like a cylinder) and the colours of the text */
static void MakeColours(void)
{
    for (ULONG x = 0; x < TEX_PERIOD; x++)
    {
        ULONG r, g, b;
        LONG d = (LONG)(x % BAR_WIDTH) - BAR_WIDTH / 2;
        ULONG shade = 255 - (ULONG)(d * d) * 255 / ((BAR_WIDTH / 2) * (BAR_WIDTH / 2));

        Hue((x / BAR_WIDTH) * (256 * BAR_WIDTH / TEX_PERIOD), &r, &g, &b);
        shade = 40 + shade * 215 / 255;
        tex[x] = Pack(r * shade / 255, g * shade / 255, b * shade / 255);
    }

    ink[INK_NONE] = 0;
    ink[INK_SHADOW] = Pack(0, 0, 0);
    ink[INK_WHITE] = Pack(255, 255, 255);
    ink[INK_GREY] = Pack(160, 160, 160);
    ink[INK_YELLOW] = Pack(255, 220, 40);
    ink[INK_GREEN] = Pack(80, 255, 120);
}

/* ------------------------------------------------------------------------------------------------------------ */
/* Text                                                                                                         */
/* ------------------------------------------------------------------------------------------------------------ */

/* The glyphs of topaz 8: drawn once by graphics.library into a one-plane bitmap, then read back */
static BOOL MakeFont(void)
{
    struct TextAttr ta = { (STRPTR)"topaz.font", 8, FS_NORMAL, FPF_ROMFONT };
    struct TextFont *font;
    struct BitMap *bm;
    struct RastPort rp;
    char chars[96];
    BOOL ok = FALSE;

    font = OpenFont(&ta);
    if (font == NULL)
        return FALSE;

    bm = AllocBitMap(96 * 8, 8, 1, BMF_CLEAR, NULL);
    if (bm != NULL)
    {
        for (ULONG i = 0; i < 96; i++)
            chars[i] = (char)(32 + i);
        chars[95] = ' ';                        /* 127 is not printable */

        InitRastPort(&rp);
        rp.BitMap = bm;
        SetFont(&rp, font);
        SetAPen(&rp, 1);
        SetDrMd(&rp, JAM1);
        Move(&rp, 0, font->tf_Baseline);
        Text(&rp, (STRPTR)chars, 96);
        WaitBlit();

        for (ULONG c = 0; c < 96; c++)
            for (ULONG y = 0; y < 8; y++)
                glyph[c][y] = ((UBYTE *)bm->Planes[0])[y * bm->BytesPerRow + c];

        FreeBitMap(bm);
        ok = TRUE;
    }

    CloseFont(font);
    return ok;
}

static ULONG TextWidth(const char *s, ULONG scale)
{
    return (ULONG)strlen(s) * 8 * scale;
}

/* A string into the mask at (x, y), y counted from the top of the top panel; scale 1 or 2, with a shadow */
static void PutText(LONG x, LONG y, const char *s, ULONG scale, UBYTE colour)
{
    for (ULONG pass = 0; pass < 2; pass++)
    {
        LONG ox = x + (pass == 0 ? (LONG)scale : 0);        /* the shadow first, one dot down and right */
        LONG oy = y + (pass == 0 ? (LONG)scale : 0);
        UBYTE ink_index = pass == 0 ? INK_SHADOW : colour;

        for (const char *p = s; *p; p++, ox += 8 * (LONG)scale)
        {
            UBYTE c = (UBYTE)*p;
            const UBYTE *gl = glyph[(c >= 32 && c < 127) ? c - 32 : 0];

            for (ULONG gy = 0; gy < 8; gy++)
            {
                for (ULONG gx = 0; gx < 8; gx++)
                {
                    if ((gl[gy] & (0x80 >> gx)) == 0)
                        continue;

                    for (ULONG sy = 0; sy < scale; sy++)
                    {
                        LONG py = oy + (LONG)(gy * scale + sy);
                        LONG px = ox + (LONG)(gx * scale);

                        if (py < 0 || py >= PANEL_LINES || px < 0 || px + (LONG)scale > (LONG)width)
                            continue;

                        for (ULONG sx = 0; sx < scale; sx++)
                            mask[py * width + px + sx] = ink_index;
                        if (px < span_first[py])
                            span_first[py] = (UWORD)px;
                        if (px + (LONG)scale - 1 > span_last[py])
                            span_last[py] = (UWORD)(px + scale - 1);
                    }
                }
            }
        }
    }
}

/* The text of both panels: title and meters on top, keys at the bottom */
static void MakeText(ULONG mode, const struct Meter *m)
{
    LONG x, y;

    memset(mask, INK_NONE, PANEL_LINES * width);
    for (y = 0; y < PANEL_LINES; y++)
    {
        span_first[y] = 0xffff;
        span_last[y] = 0;
    }

    PutText(12, 8, "TearTestP96", 2, INK_WHITE);
    PutText(12, 32, "Picasso96 vertical blank", 1, INK_GREY);
    PutText(12, 42, "test for PiStorm / Emu68", 1, INK_GREY);
    PutText((LONG)(width - TextWidth(m->fps, 2) - 12), 8, m->fps, 2, INK_YELLOW);
    PutText((LONG)(width - TextWidth(m->cpu_free, 2) - 12), 30, m->cpu_free, 2, INK_GREEN);

    x = 12;
    y = TOP_PANEL + 8;
    PutText(x, y, "[SPACE]", 2, INK_WHITE);
    x += (LONG)TextWidth("[SPACE] ", 2);
    for (ULONG i = 0; i < MODE_COUNT; i++)
    {
        PutText(x, y, mode_name[i], 2, i == mode ? INK_YELLOW : INK_GREY);
        x += (LONG)TextWidth(mode_name[i], 2) + 16;
    }
    PutText((LONG)(width - TextWidth("[ESC] Quit", 1) - 12), y + 4, "[ESC] Quit", 1, INK_GREY);
}

/* ------------------------------------------------------------------------------------------------------------ */
/* Picture                                                                                                      */
/* ------------------------------------------------------------------------------------------------------------ */

/* Panel line pl over the dark bars: darkrow itself when it has no text */
static const ULONG *PanelLine(ULONG pl)
{
    const UBYTE *m = &mask[pl * width];

    if (span_first[pl] > span_last[pl])
        return darkrow;

    memcpy(textrow, darkrow, width * 4);
    for (ULONG x = span_first[pl]; x <= span_last[pl]; x++)
        if (m[x] != INK_NONE)
            textrow[x] = ink[m[x]];

    return textrow;
}

/* The next frame into fast memory: bars scrolled by 'scroll', the white bar at 'white', the panels over them */
static void ComposeFrame(ULONG scroll, LONG white)
{
    ULONG y;

    for (ULONG x = 0; x < width; x++)
        row[x] = tex[(x + scroll) & (TEX_PERIOD - 1)];
    for (LONG x = white; x < white + WHITE_WIDTH; x++)
        if (x >= 0 && x < (LONG)width)
            row[x] = ink[INK_WHITE];
    for (ULONG x = 0; x < width; x++)
        darkrow[x] = Dark(row[x]);

    for (y = 0; y < TOP_PANEL; y++)
        CopyMemQuick((APTR)PanelLine(y), &frame[y * width], width * 4);
    for (; y < height - BOTTOM_PANEL; y++)
        CopyMemQuick(row, &frame[y * width], width * 4);
    for (; y < height; y++)
        CopyMemQuick((APTR)PanelLine(y - (height - BOTTOM_PANEL) + TOP_PANEL), &frame[y * width], width * 4);
}

/* The frame to the screen, as SDL or MiniGL would */
static void CopyFrame(struct Screen *scr)
{
    struct RenderInfo ri;

    ri.Memory = frame;
    ri.BytesPerRow = (WORD)(width * 4);
    ri.pad = 0;
    ri.RGBFormat = (RGBFTYPE)pixfmt;

    p96WritePixelArray(&ri, 0, 0, &scr->RastPort, 0, 0, (UWORD)width, (UWORD)height);
}

/* ------------------------------------------------------------------------------------------------------------ */
/* CPU free: the idle task                                                                                      */
/* ------------------------------------------------------------------------------------------------------------ */

/* At the lowest priority, it runs only when every other task waits */
static void IdleTask(void)
{
    struct ExecBase *SysBase = *(struct ExecBase **)4;

    while (!idle_stop)
        idle_count++;

    /* Forbid() keeps it running until it is gone: the parent frees its memory once signalled */
    Forbid();
    Signal(idle_parent, 1UL << idle_sig);
}

/* mem holds the Task structure followed by its stack */
static void StartIdle(APTR mem, struct Task *parent)
{
    struct Task *t = (struct Task *)mem;
    UBYTE *stack = (UBYTE *)mem + sizeof(struct Task);

    idle_parent = parent;

    t->tc_Node.ln_Type = NT_TASK;
    t->tc_Node.ln_Pri = -128;
    t->tc_Node.ln_Name = (char *)"TearTestP96 idle";
    t->tc_SPLower = stack;
    t->tc_SPUpper = stack + IDLE_STACK;
    t->tc_SPReg = stack + IDLE_STACK;

    /* RemTask() walks tc_MemEntry when the task ends: it must be an empty list */
    t->tc_MemEntry.lh_Head = (struct Node *)&t->tc_MemEntry.lh_Tail;
    t->tc_MemEntry.lh_Tail = NULL;
    t->tc_MemEntry.lh_TailPred = (struct Node *)&t->tc_MemEntry.lh_Head;

    AddTask(t, (APTR)IdleTask, NULL);
}

static void StopIdle(void)
{
    idle_stop = TRUE;
    Wait(1UL << idle_sig);          /* this task sleeps, the idle one runs, sees the flag and signals */
}

/* ------------------------------------------------------------------------------------------------------------ */
/* Measures                                                                                                     */
/* ------------------------------------------------------------------------------------------------------------ */

static unsigned long long Ticks(const struct EClockVal *e)
{
    return ((unsigned long long)e->ev_hi << 32) | e->ev_lo;
}

static char *PutNum(char *p, ULONG v)
{
    char tmp[12];
    int n = 0;

    do { tmp[n++] = (char)('0' + v % 10); v /= 10; } while (v != 0);
    while (n > 0)
        *p++ = tmp[--n];
    return p;
}

/* How fast the idle task counts while this task sleeps: the 100% of CPU free */
static void Calibrate(struct Meter *m)
{
    struct EClockVal t0, t1;
    ULONG c0 = idle_count;

    ReadEClock(&t0);
    Delay(CALIBRATE_TICKS);
    ReadEClock(&t1);
    m->idle_rate = (ULONG)((unsigned long long)(idle_count - c0) * m->freq / (Ticks(&t1) - Ticks(&t0)));
}

/* Restart the figures on the screen (at the start and at each change of mode) */
static void ResetMeter(struct Meter *m)
{
    ReadEClock(&m->last);
    m->frames = 0;
    m->idle_at_last = idle_count;
    m->free_sum = m->free_n = 0;
    strcpy(m->fps, "-- FPS");
    strcpy(m->cpu_free, "CPU free --");
}

/* Called after each frame; returns TRUE when the strings changed (twice a second) */
static BOOL UpdateMeter(struct Meter *m)
{
    struct EClockVal now;
    ULONG elapsed, percent, fps10;
    char *p;

    m->frames++;
    m->total_frames++;
    ReadEClock(&now);
    m->run_ticks = Ticks(&now) - Ticks(&m->start);

    elapsed = (ULONG)(Ticks(&now) - Ticks(&m->last));
    if (elapsed < m->freq / 2)
        return FALSE;

    percent = m->idle_rate
        ? (ULONG)((unsigned long long)(idle_count - m->idle_at_last) * m->freq * 100 / elapsed / m->idle_rate)
        : 0;
    if (percent > 100)
        percent = 100;
    fps10 = (ULONG)((unsigned long long)m->frames * m->freq * 10 / elapsed);

    p = PutNum(m->fps, fps10 / 10);
    *p++ = '.';
    *p++ = (char)('0' + fps10 % 10);
    strcpy(p, " FPS");

    strcpy(m->cpu_free, "CPU free ");
    p = PutNum(m->cpu_free + 9, percent);
    strcpy(p, "%");

    m->free_sum += percent;
    m->free_n++;
    m->frames = 0;
    m->last = now;
    m->idle_at_last = idle_count;
    return TRUE;
}

/* The averages of the last mode, for SECONDS runs */
static void PrintSummary(const struct Meter *m, ULONG mode)
{
    ULONG fps10, compose_us, copy_us;

    if (m->total_frames == 0 || m->run_ticks == 0)
        return;

    fps10 = (ULONG)((unsigned long long)m->total_frames * m->freq * 10 / m->run_ticks);
    compose_us = (ULONG)(m->compose_ticks * 1000000 / m->freq / m->total_frames);
    copy_us = (ULONG)(m->copy_ticks * 1000000 / m->freq / m->total_frames);

    Printf((CONST_STRPTR)"TearTestP96: %ldx%ld, %s, %ld frames in %ld ms: %ld.%ld FPS, %ld us to compose a frame, "
           "%ld us to copy it, CPU free %ld%%\n",
           (LONG)width, (LONG)height, (LONG)mode_name[mode], (LONG)m->total_frames,
           (LONG)(m->run_ticks * 1000 / m->freq), (LONG)(fps10 / 10), (LONG)(fps10 % 10),
           (LONG)compose_us, (LONG)copy_us, (LONG)(m->free_n ? m->free_sum / m->free_n : 0));
}

/* ------------------------------------------------------------------------------------------------------------ */
/* Screen                                                                                                       */
/* ------------------------------------------------------------------------------------------------------------ */

/*
    The 32-bit mode nearest to the size asked. p96BestModeIDTags() of the legacy Picasso96 returns a mode of any
    size when no depth is given, and none with a depth of 24, so the mode list is read instead.
*/
static ULONG FindMode(ULONG want_w, ULONG want_h)
{
    const ULONG formats = RGBFF_A8R8G8B8 | RGBFF_B8G8R8A8 | RGBFF_R8G8B8A8 | RGBFF_A8B8G8R8;
    struct List *list = p96AllocModeListTags(P96MA_FormatsAllowed, formats, TAG_DONE);
    ULONG best = (ULONG)INVALID_ID, best_dist = 0xffffffff;

    if (list == NULL)
        return best;

    for (struct Node *n = list->lh_Head; n->ln_Succ != NULL; n = n->ln_Succ)
    {
        struct P96Mode *pm = (struct P96Mode *)n;
        ULONG fmt = p96GetModeIDAttr(pm->DisplayID, P96IDA_RGBFORMAT);
        LONG dw = (LONG)pm->Width - (LONG)want_w;
        LONG dh = (LONG)pm->Height - (LONG)want_h;
        ULONG dist = (ULONG)(dw < 0 ? -dw : dw) + (ULONG)(dh < 0 ? -dh : dh);

        if ((formats & (1UL << fmt)) != 0 && dist < best_dist)
        {
            best_dist = dist;
            best = pm->DisplayID;
        }
    }

    p96FreeModeList(list);
    return best;
}

/* The screen, and a borderless window on it for the keys, with the mouse pointer hidden */
static BOOL OpenDisplay(struct App *app, ULONG id)
{
    app->screen = p96OpenScreenTags(P96SA_DisplayID, id,
                                    P96SA_Width, width,
                                    P96SA_Height, height,
                                    P96SA_RGBFormat, pixfmt,
                                    P96SA_AutoScroll, FALSE,
                                    P96SA_ShowTitle, FALSE,
                                    P96SA_Quiet, TRUE,
                                    P96SA_Title, (ULONG)"TearTestP96",
                                    TAG_DONE);
    if (app->screen == NULL)
    {
        Printf((CONST_STRPTR)"TearTestP96: cannot open a %ldx%ld screen\n", (LONG)width, (LONG)height);
        return FALSE;
    }

    app->window = OpenWindowTags(NULL,
                                 WA_CustomScreen, (ULONG)app->screen,
                                 WA_Left, 0, WA_Top, 0,
                                 WA_Width, width, WA_Height, height,
                                 WA_Backdrop, TRUE,
                                 WA_Borderless, TRUE,
                                 WA_Activate, TRUE,
                                 WA_RMBTrap, TRUE,
                                 WA_IDCMP, IDCMP_RAWKEY,
                                 TAG_DONE);
    if (app->window == NULL)
        return FALSE;

    SetPointer(app->window, app->blank_pointer, 1, 1, 0, 0);
    return TRUE;
}

/* ------------------------------------------------------------------------------------------------------------ */
/* Main                                                                                                         */
/* ------------------------------------------------------------------------------------------------------------ */

/* NoWait never waits: it runs below the other tasks, so that the system (input, network) keeps running */
static void ApplyMode(struct App *app, ULONG mode)
{
    SetTaskPri(app->me, mode == MODE_NOWAIT ? -1 : 0);
}

/* Reads the arguments; returns FALSE on a wrong one */
static BOOL ParseArgs(struct App *app, ULONG *mode, ULONG *seconds, ULONG *want_w, ULONG *want_h)
{
    LONG *args[4] = { NULL, NULL, NULL, NULL };

    app->rdargs = ReadArgs((CONST_STRPTR)"MODE/K,SECONDS/N,WIDTH/N,HEIGHT/N", (LONG *)args, NULL);
    if (app->rdargs == NULL)
    {
        PrintFault(IoErr(), (CONST_STRPTR)"TearTestP96");
        return FALSE;
    }

    if (args[0] != NULL)
    {
        for (*mode = 0; *mode < MODE_COUNT; (*mode)++)
            if (Stricmp((CONST_STRPTR)args[0], (CONST_STRPTR)mode_name[*mode]) == 0)
                break;
        if (*mode == MODE_COUNT)
        {
            Printf((CONST_STRPTR)"TearTestP96: MODE is WaitBOVP, WaitTOF or NoWait\n");
            return FALSE;
        }
    }
    if (args[1] != NULL)
        *seconds = (ULONG)*args[1];
    if (args[2] != NULL)
        *want_w = (ULONG)*args[2];
    if (args[3] != NULL)
        *want_h = (ULONG)*args[3];

    return TRUE;
}

/* Everything the program needs before the screen opens */
static BOOL Setup(struct App *app, struct Meter *m, ULONG want_w, ULONG want_h, ULONG *id)
{
    P96Base = OpenLibrary((CONST_STRPTR)"Picasso96API.library", 2);
    if (P96Base == NULL)
    {
        Printf((CONST_STRPTR)"TearTestP96: needs Picasso96API.library 2\n");
        return FALSE;
    }

    if (OpenDevice((CONST_STRPTR)TIMERNAME, UNIT_MICROHZ, (struct IORequest *)&app->timer, 0) != 0)
        return FALSE;
    app->timer_open = TRUE;
    TimerBase = app->timer.tr_node.io_Device;
    m->freq = ReadEClock(&m->start);

    *id = FindMode(want_w, want_h);
    if (*id == (ULONG)INVALID_ID)
    {
        Printf((CONST_STRPTR)"TearTestP96: no 32-bit screen mode (add one with Picasso96Mode)\n");
        return FALSE;
    }
    width = p96GetModeIDAttr(*id, P96IDA_WIDTH);
    height = p96GetModeIDAttr(*id, P96IDA_HEIGHT);
    pixfmt = p96GetModeIDAttr(*id, P96IDA_RGBFORMAT);

    tex = AllocVec(TEX_PERIOD * 4, MEMF_ANY);
    row = AllocVec(width * 4, MEMF_ANY);
    darkrow = AllocVec(width * 4, MEMF_ANY);
    textrow = AllocVec(width * 4, MEMF_ANY);
    frame = AllocVec(width * height * 4, MEMF_ANY);
    mask = AllocVec(PANEL_LINES * width, MEMF_ANY);
    app->blank_pointer = AllocVec(4 * sizeof(UWORD), MEMF_CHIP | MEMF_CLEAR);
    app->idle_mem = AllocVec(sizeof(struct Task) + IDLE_STACK, MEMF_CLEAR);
    idle_sig = AllocSignal(-1);
    if (tex == NULL || row == NULL || darkrow == NULL || textrow == NULL || frame == NULL || mask == NULL ||
        app->blank_pointer == NULL || app->idle_mem == NULL || idle_sig == -1)
        return FALSE;

    if (!MakeFont())
        return FALSE;
    MakeColours();

    StartIdle(app->idle_mem, app->me);
    app->idle_running = TRUE;
    Calibrate(m);

    return TRUE;
}

static void Cleanup(struct App *app)
{
    SetTaskPri(app->me, app->old_pri);
    if (app->idle_running)
        StopIdle();
    if (app->window != NULL)
    {
        ClearPointer(app->window);
        CloseWindow(app->window);
    }
    if (app->screen != NULL)
        p96CloseScreen(app->screen);
    if (idle_sig != -1)
        FreeSignal(idle_sig);
    FreeVec(app->idle_mem);
    FreeVec(app->blank_pointer);
    FreeVec(mask);
    FreeVec(frame);
    FreeVec(textrow);
    FreeVec(darkrow);
    FreeVec(row);
    FreeVec(tex);
    if (app->timer_open)
        CloseDevice((struct IORequest *)&app->timer);
    if (P96Base != NULL)
        CloseLibrary(P96Base);
    if (app->rdargs != NULL)
        FreeArgs(app->rdargs);
}

/* Handles the keys; returns FALSE on [ESC] */
static BOOL HandleKeys(struct App *app, struct Meter *m, ULONG *mode)
{
    struct IntuiMessage *msg;
    BOOL running = TRUE;

    while ((msg = (struct IntuiMessage *)GetMsg(app->window->UserPort)) != NULL)
    {
        if (msg->Class == IDCMP_RAWKEY)
        {
            if (msg->Code == RAWKEY_ESC)
                running = FALSE;
            else if (msg->Code == RAWKEY_SPACE)
            {
                *mode = (*mode + 1) % MODE_COUNT;
                ApplyMode(app, *mode);
                ResetMeter(m);
                MakeText(*mode, m);
            }
        }
        ReplyMsg((struct Message *)msg);
    }

    return running;
}

/* Compose, wait, copy, until [ESC], Ctrl-C or the end of SECONDS */
static void Run(struct App *app, struct Meter *m, ULONG mode, ULONG seconds)
{
    ULONG scroll = 0;
    LONG white = 0, white_step = WHITE_SPEED;
    BOOL running = TRUE;

    ApplyMode(app, mode);
    ReadEClock(&m->start);
    ResetMeter(m);
    MakeText(mode, m);

    while (running)
    {
        struct EClockVal t0, t1;

        ReadEClock(&t0);
        ComposeFrame(scroll, white);
        ReadEClock(&t1);
        m->compose_ticks += Ticks(&t1) - Ticks(&t0);

        if (mode == MODE_WAITBOVP)
            WaitBOVP(&app->screen->ViewPort);
        else if (mode == MODE_WAITTOF)
            WaitTOF();

        ReadEClock(&t0);
        CopyFrame(app->screen);
        ReadEClock(&t1);
        m->copy_ticks += Ticks(&t1) - Ticks(&t0);

        /* Move the bars and the white bar for the next frame */
        scroll = (scroll + SCROLL_SPEED) & (TEX_PERIOD - 1);
        white += white_step;
        if (white <= 0 || white + WHITE_WIDTH >= (LONG)width)
        {
            white_step = -white_step;
            white = white <= 0 ? 0 : (LONG)width - WHITE_WIDTH;
        }

        if (UpdateMeter(m))
            MakeText(mode, m);

        running = HandleKeys(app, m, &mode);
        if (seconds != 0 && m->run_ticks >= (unsigned long long)seconds * m->freq)
            running = FALSE;
        if (SetSignal(0, 0) & SIGBREAKF_CTRL_C)
            running = FALSE;
    }

    if (seconds != 0)
        PrintSummary(m, mode);
}

int main(void)
{
    struct App app;
    struct Meter meter;
    ULONG mode = MODE_WAITBOVP, seconds = 0, id;
    ULONG want_w = DEFAULT_WIDTH, want_h = DEFAULT_HEIGHT;
    int rc = RETURN_FAIL;

    memset(&app, 0, sizeof(app));
    memset(&meter, 0, sizeof(meter));
    app.me = FindTask(NULL);
    app.old_pri = app.me->tc_Node.ln_Pri;

    if (ParseArgs(&app, &mode, &seconds, &want_w, &want_h) &&
        Setup(&app, &meter, want_w, want_h, &id) &&
        OpenDisplay(&app, id))
    {
        Run(&app, &meter, mode, seconds);
        rc = RETURN_OK;
    }

    Cleanup(&app);
    return rc;
}
