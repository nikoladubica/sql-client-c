// Resizable splits for libui-ng: an NSSplitView wrapped as a libui
// container control (libui-ng has no splitter of its own). Two shapes: a
// sidebar (side by side, fixed sidebar width) and a top/bottom split
// (divider at a fraction of the height).
#import <Cocoa/Cocoa.h>

#include "ui.h"
#include "ui_darwin.h"
#include "splitview.h"

#define SqlcSplitSignature 0x53717370 /* "Sqsp" */
#define SqlcSplit(this) ((SqlcSplit *) (this))

static const CGFloat MIN_MAIN_WIDTH = 240;

typedef struct SqlcSplit SqlcSplit;

@interface SqlcSplitView : NSSplitView <NSSplitViewDelegate> {
@public
    BOOL stacked;        /* YES = top/bottom panes, NO = sidebar | main */
    CGFloat wantedSize;  /* sidebar: first pane width to apply on the first layout */
    CGFloat wantedFrac;  /* stacked: top pane's share of the height, 0..1 */
    CGFloat minFirst;    /* minimum width/height of pane 0 */
    CGFloat minSecond;   /* minimum width/height of pane 1 */
    NSString *saveKey;   /* retained; nil = don't persist */
    BOOL positioned;     /* initial position applied; saving is allowed */
}
@end

@implementation SqlcSplitView
- (void)dealloc {
    [saveKey release];
    [super dealloc];
}

/* Length along the axis the divider moves on. */
- (CGFloat)extent {
    return stacked ? NSHeight([self bounds]) : NSWidth([self bounds]);
}

/* Room left for both panes once the divider is taken out. */
- (CGFloat)paneSpace {
    return [self extent] - [self dividerThickness];
}

/* Clamp a divider position so that neither pane goes below its minimum. */
- (CGFloat)clampPosition:(CGFloat)pos {
    CGFloat max = [self paneSpace] - minSecond;
    if (pos > max) pos = max;
    if (pos < minFirst) pos = minFirst;
    return pos;
}

- (void)layout {
    [super layout];
    // Panes have no size until the first layout pass, so apply the starting
    // position here, once. A stacked split waits until both minimums fit, so
    // a tiny first pass can't pin the divider against one edge.
    if (positioned) return;
    CGFloat space = [self paneSpace];
    if (space <= 0) return;
    if (stacked && space < minFirst + minSecond) return;
    CGFloat pos = stacked ? floor(space * wantedFrac) : wantedSize;
    [self setPosition:[self clampPosition:pos] ofDividerAtIndex:0];
    positioned = YES;
}

// Constraints already enforce these; the delegate keeps the drag itself from
// overshooting so the divider doesn't jump back.
- (CGFloat)splitView:(NSSplitView *)sv constrainMinCoordinate:(CGFloat)proposed ofSubviewAt:(NSInteger)i {
    (void)sv; (void)i;
    return proposed < minFirst ? minFirst : proposed;
}

- (CGFloat)splitView:(NSSplitView *)sv constrainMaxCoordinate:(CGFloat)proposed ofSubviewAt:(NSInteger)i {
    (void)sv; (void)i;
    CGFloat max = [self paneSpace] - minSecond;
    return proposed > max ? max : proposed;
}

- (BOOL)splitView:(NSSplitView *)sv canCollapseSubview:(NSView *)v {
    (void)sv; (void)v;
    return NO;
}

- (void)splitViewDidResizeSubviews:(NSNotification *)n {
    (void)n;
    if (!positioned || saveKey == nil) return;
    NSArray *panes = [self arrangedSubviews];
    if ([panes count] < 1) return;
    NSRect f = [[panes objectAtIndex:0] frame];
    if (stacked) {
        // Stored as a fraction so it fits any window height on the next launch.
        CGFloat space = [self paneSpace];
        CGFloat h = NSHeight(f);
        if (space >= minFirst + minSecond && h >= minFirst && space - h >= minSecond)
            [[NSUserDefaults standardUserDefaults] setDouble:h / space forKey:saveKey];
    } else if (NSWidth(f) >= minFirst) {
        [[NSUserDefaults standardUserDefaults] setDouble:NSWidth(f) forKey:saveKey];
    }
}
@end

struct SqlcSplit {
    uiDarwinControl c;
    SqlcSplitView *split;
    uiControl *children[2];
    NSLayoutPriority horzHuggingPri, vertHuggingPri;
};

static void SqlcSplitDestroy(uiControl *c) {
    SqlcSplit *s = SqlcSplit(c);
    for (int i = 0; i < 2; i++) {
        uiControlSetParent(s->children[i], NULL);
        uiDarwinControlSetSuperview(uiDarwinControl(s->children[i]), nil);
        uiControlDestroy(s->children[i]);
    }
    [s->split setDelegate:nil];
    [s->split release];
    uiFreeControl(c);
}

// libui's default implementations ignore some of their parameters.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-parameter"
uiDarwinControlDefaultHandle(SqlcSplit, split)
uiDarwinControlDefaultParent(SqlcSplit, split)
uiDarwinControlDefaultSetParent(SqlcSplit, split)
uiDarwinControlDefaultToplevel(SqlcSplit, split)
uiDarwinControlDefaultVisible(SqlcSplit, split)
uiDarwinControlDefaultShow(SqlcSplit, split)
uiDarwinControlDefaultHide(SqlcSplit, split)
uiDarwinControlDefaultEnabled(SqlcSplit, split)
uiDarwinControlDefaultEnable(SqlcSplit, split)
uiDarwinControlDefaultDisable(SqlcSplit, split)
uiDarwinControlDefaultSetSuperview(SqlcSplit, split)
uiDarwinControlDefaultChildEdgeHuggingChanged(SqlcSplit, split)
uiDarwinControlDefaultChildVisibilityChanged(SqlcSplit, split)
#pragma clang diagnostic pop

static void SqlcSplitSyncEnableState(uiDarwinControl *c, int enabled) {
    SqlcSplit *s = SqlcSplit(c);
    if (uiDarwinShouldStopSyncEnableState(c, enabled)) return;
    for (int i = 0; i < 2; i++)
        uiDarwinControlSyncEnableState(uiDarwinControl(s->children[i]), enabled);
}

// Like uiGroup: low hugging, so the split fills whatever its parent gives it.
static BOOL SqlcSplitHugsTrailingEdge(uiDarwinControl *c) {
    return SqlcSplit(c)->horzHuggingPri < NSLayoutPriorityWindowSizeStayPut;
}

static BOOL SqlcSplitHugsBottom(uiDarwinControl *c) {
    return SqlcSplit(c)->vertHuggingPri < NSLayoutPriorityWindowSizeStayPut;
}

static NSLayoutPriority SqlcSplitHuggingPriority(uiDarwinControl *c, NSLayoutConstraintOrientation o) {
    SqlcSplit *s = SqlcSplit(c);
    return o == NSLayoutConstraintOrientationHorizontal ? s->horzHuggingPri : s->vertHuggingPri;
}

static void SqlcSplitSetHuggingPriority(uiDarwinControl *c, NSLayoutPriority p, NSLayoutConstraintOrientation o) {
    SqlcSplit *s = SqlcSplit(c);
    if (o == NSLayoutConstraintOrientationHorizontal) s->horzHuggingPri = p;
    else s->vertHuggingPri = p;
    uiDarwinNotifyEdgeHuggingChanged(c);
}

static void pin(NSView *pane, NSView *child, NSLayoutAttribute attr, CGFloat inset) {
    [pane addConstraint:[NSLayoutConstraint constraintWithItem:child attribute:attr
                                                     relatedBy:NSLayoutRelationEqual
                                                        toItem:pane attribute:attr
                                                    multiplier:1 constant:inset]];
}

/* Put child in a plain pane view that fills it; the split view sizes the pane.
 * `before` / `after` inset the child at the start / end of the split axis
 * (left / right, or top / bottom when stacked). */
static NSView *add_pane(SqlcSplit *s, int i, uiControl *child, BOOL stacked,
                        CGFloat before, CGFloat after) {
    NSView *pane = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 100, 100)];
    s->children[i] = child;
    uiControlSetParent(child, uiControl(s));
    uiDarwinControlSetSuperview(uiDarwinControl(child), pane);
    uiDarwinControlSyncEnableState(uiDarwinControl(child), uiControlEnabledToUser(uiControl(s)));
    // Fill the pane rather than hugging the child's content.
    uiDarwinControlSetHuggingPriority(uiDarwinControl(child), NSLayoutPriorityDefaultLow, NSLayoutConstraintOrientationHorizontal);
    uiDarwinControlSetHuggingPriority(uiDarwinControl(child), NSLayoutPriorityDefaultLow, NSLayoutConstraintOrientationVertical);

    NSView *cv = (NSView *) uiControlHandle(child);
    pin(pane, cv, NSLayoutAttributeLeading, stacked ? 0 : before);
    pin(pane, cv, NSLayoutAttributeTrailing, stacked ? 0 : -after);
    pin(pane, cv, NSLayoutAttributeTop, stacked ? before : 0);
    pin(pane, cv, NSLayoutAttributeBottom, stacked ? -after : 0);

    [s->split addSubview:pane];
    [pane release]; /* retained by the split view */
    return pane;
}

/* pane's width (or height) >= min, so a window resize can't squash it either. */
static void min_size(NSView *pane, NSLayoutAttribute attr, CGFloat min) {
    [pane addConstraint:[NSLayoutConstraint constraintWithItem:pane attribute:attr
                                                     relatedBy:NSLayoutRelationGreaterThanOrEqual
                                                        toItem:nil attribute:NSLayoutAttributeNotAnAttribute
                                                    multiplier:1 constant:min]];
}

/* The shared part of both constructors; save_key may be NULL. */
static SqlcSplit *new_split(uiControl *first, uiControl *second, BOOL stacked,
                            CGFloat min_first, CGFloat min_second, const char *save_key) {
    SqlcSplit *s;
    uiDarwinNewControl(SqlcSplit, s);
    s->horzHuggingPri = NSLayoutPriorityDefaultLow;
    s->vertHuggingPri = NSLayoutPriorityDefaultLow;

    SqlcSplitView *sv = [[SqlcSplitView alloc] initWithFrame:NSZeroRect];
    [sv setVertical:!stacked]; /* NSSplitView's "vertical" means a vertical divider */
    [sv setDividerStyle:NSSplitViewDividerStyleThin];
    [sv setDelegate:sv];
    sv->stacked = stacked;
    sv->minFirst = min_first;
    sv->minSecond = min_second;
    if (save_key) sv->saveKey = [[NSString alloc] initWithUTF8String:save_key];
    s->split = sv;

    // A little space on each side of the divider, like a padded uiBox.
    CGFloat gap = uiDarwinPaddingAmount(NULL) / 2;
    NSLayoutAttribute size = stacked ? NSLayoutAttributeHeight : NSLayoutAttributeWidth;
    min_size(add_pane(s, 0, first, stacked, 0, gap), size, min_first);
    min_size(add_pane(s, 1, second, stacked, gap, 0), size, min_second);
    return s;
}

uiControl *sqlc_new_sidebar_split(uiControl *side, uiControl *main,
                                  int initial_width, int min_width,
                                  const char *save_key) {
    SqlcSplit *s = new_split(side, main, NO, min_width, MIN_MAIN_WIDTH, save_key);
    SqlcSplitView *sv = s->split;
    sv->wantedSize = initial_width;
    if (sv->saveKey) {
        double saved = [[NSUserDefaults standardUserDefaults] doubleForKey:sv->saveKey];
        if (saved >= min_width) sv->wantedSize = saved;
    }

    // When the window is resized, the sidebar keeps its width and the main
    // pane absorbs the change.
    [sv setHoldingPriority:NSLayoutPriorityDefaultLow + 10 forSubviewAtIndex:0];
    [sv setHoldingPriority:NSLayoutPriorityDefaultLow forSubviewAtIndex:1];

    return uiControl(s);
}

uiControl *sqlc_new_vertical_split(uiControl *top, uiControl *bottom,
                                   double initial_fraction, int min_height,
                                   const char *save_key) {
    if (min_height < 1) min_height = 1;
    SqlcSplit *s = new_split(top, bottom, YES, min_height, min_height, save_key);
    SqlcSplitView *sv = s->split;
    // A bad fraction (NaN, out of range) falls back to half.
    double frac = initial_fraction > 0 && initial_fraction < 1 ? initial_fraction : 0.5;
    if (sv->saveKey) {
        // A stored 0 means "nothing saved"; anything outside (0, 1) is ignored.
        // Layout clamps the result to both minimum heights anyway.
        double saved = [[NSUserDefaults standardUserDefaults] doubleForKey:sv->saveKey];
        if (saved > 0 && saved < 1) frac = saved;
    }
    sv->wantedFrac = frac;

    // When the window is resized, both panes grow or shrink together.
    [sv setHoldingPriority:NSLayoutPriorityDefaultLow forSubviewAtIndex:0];
    [sv setHoldingPriority:NSLayoutPriorityDefaultLow forSubviewAtIndex:1];

    return uiControl(s);
}

/* Size of pane 0 along the split axis. */
static int first_pane_size(uiControl *split) {
    SqlcSplitView *sv = SqlcSplit(split)->split;
    NSArray *panes = [sv arrangedSubviews];
    if (![panes count]) return 0;
    NSRect f = [[panes objectAtIndex:0] frame];
    return (int) (sv->stacked ? NSHeight(f) : NSWidth(f));
}

/* Move the divider; the clamp keeps both panes at or above their minimums. */
static void set_first_pane_size(uiControl *split, int size) {
    SqlcSplitView *sv = SqlcSplit(split)->split;
    [sv setPosition:[sv clampPosition:size] ofDividerAtIndex:0];
    [[sv window] layoutIfNeeded];
}

int sqlc_split_sidebar_width(uiControl *split) { return first_pane_size(split); }
void sqlc_split_set_sidebar_width(uiControl *split, int width) { set_first_pane_size(split, width); }

int sqlc_split_top_height(uiControl *split) { return first_pane_size(split); }
void sqlc_split_set_top_height(uiControl *split, int height) { set_first_pane_size(split, height); }

void sqlc_table_fill_width(uiTable *t) {
    NSScrollView *sv = (NSScrollView *) uiControlHandle(uiControl(t));
    NSTableView *tv = [sv documentView];
    if (![tv isKindOfClass:[NSTableView class]]) return;
    [tv setColumnAutoresizingStyle:NSTableViewLastColumnOnlyAutoresizingStyle];
    for (NSTableColumn *col in [tv tableColumns]) {
        [col setMinWidth:20];
        [col setResizingMask:NSTableColumnAutoresizingMask | NSTableColumnUserResizingMask];
    }
    [tv sizeLastColumnToFit];
}
