// Resizable panes for libui-ng: an NSSplitView wrapped as a libui container
// control (libui-ng has no splitter of its own). Side by side (the sidebar)
// or stacked (the SQL editor above its results).
#import <Cocoa/Cocoa.h>

#include "ui.h"
#include "ui_darwin.h"
#include "splitview.h"

#define SqlcSplitSignature 0x53717370 /* "Sqsp" */
#define SqlcSplit(this) ((SqlcSplit *) (this))

static const CGFloat MIN_MAIN_WIDTH = 240;
static const CGFloat GRAB_MARGIN = 3; /* extra grab area on each side of a thin divider */

typedef struct SqlcSplit SqlcSplit;

@interface SqlcSplitView : NSSplitView <NSSplitViewDelegate> {
@public
    CGFloat wanted;      /* first pane's size to apply on the first layout;
                            0 or less = half of the room */
    CGFloat minFirst, minSecond;
    NSString *saveKey;   /* retained; nil = don't persist */
    BOOL positioned;     /* initial size applied; saving is allowed */
}
@end

@implementation SqlcSplitView
- (void)dealloc {
    [saveKey release];
    [super dealloc];
}

/* Size along the split direction: width side by side, height stacked. */
- (CGFloat)lengthOf:(NSRect)r {
    return [self isVertical] ? NSWidth(r) : NSHeight(r);
}

/* Room left for both panes once the divider is taken out. */
- (CGFloat)paneSpace {
    return [self lengthOf:[self bounds]] - [self dividerThickness];
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
    // size here, once - and only when both minimums fit, so a tiny first
    // pass can't pin the divider against one edge.
    if (positioned) return;
    CGFloat space = [self paneSpace];
    if (space <= 0 || space < minFirst + minSecond) return;
    CGFloat w = wanted > 0 ? wanted : floor(space / 2);
    [self setPosition:[self clampPosition:w] ofDividerAtIndex:0];
    positioned = YES;
}

// Constraints already enforce these; the delegate keeps the drag itself from
// overshooting so the divider doesn't jump back.
- (CGFloat)splitView:(NSSplitView *)sv constrainMinCoordinate:(CGFloat)proposed ofSubviewAt:(NSInteger)i {
    (void)sv; (void)i;
    return proposed < minFirst ? minFirst : proposed;
}

- (CGFloat)splitView:(NSSplitView *)sv constrainMaxCoordinate:(CGFloat)proposed ofSubviewAt:(NSInteger)i {
    (void)i;
    (void)sv;
    CGFloat max = [self paneSpace] - minSecond;
    return proposed > max ? max : proposed;
}

- (BOOL)splitView:(NSSplitView *)sv canCollapseSubview:(NSView *)v {
    (void)sv; (void)v;
    return NO;
}

// A thin divider is a 1pt line: make it easier to grab.
- (NSRect)splitView:(NSSplitView *)sv effectiveRect:(NSRect)proposed
       forDrawnRect:(NSRect)drawn ofDividerAtIndex:(NSInteger)i {
    (void)drawn; (void)i;
    return [sv isVertical] ? NSInsetRect(proposed, -GRAB_MARGIN, 0)
                           : NSInsetRect(proposed, 0, -GRAB_MARGIN);
}

- (void)splitViewDidResizeSubviews:(NSNotification *)n {
    (void)n;
    if (!positioned || saveKey == nil) return;
    NSArray *panes = [self arrangedSubviews];
    if ([panes count] < 1) return;
    CGFloat w = [self lengthOf:[[panes objectAtIndex:0] frame]];
    if (w >= minFirst && [self paneSpace] - w >= minSecond)
        [[NSUserDefaults standardUserDefaults] setDouble:w forKey:saveKey];
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
 * before/after: space between the child and the divider-facing edges. */
static NSView *add_pane(SqlcSplit *s, int i, uiControl *child, CGFloat before, CGFloat after) {
    NSView *pane = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 100, 100)];
    s->children[i] = child;
    uiControlSetParent(child, uiControl(s));
    uiDarwinControlSetSuperview(uiDarwinControl(child), pane);
    uiDarwinControlSyncEnableState(uiDarwinControl(child), uiControlEnabledToUser(uiControl(s)));
    // Fill the pane rather than hugging the child's content.
    uiDarwinControlSetHuggingPriority(uiDarwinControl(child), NSLayoutPriorityDefaultLow, NSLayoutConstraintOrientationHorizontal);
    uiDarwinControlSetHuggingPriority(uiDarwinControl(child), NSLayoutPriorityDefaultLow, NSLayoutConstraintOrientationVertical);

    NSView *cv = (NSView *) uiControlHandle(child);
    BOOL across = [s->split isVertical];
    pin(pane, cv, NSLayoutAttributeLeading, across ? before : 0);
    pin(pane, cv, NSLayoutAttributeTrailing, across ? -after : 0);
    pin(pane, cv, NSLayoutAttributeTop, across ? 0 : before);
    pin(pane, cv, NSLayoutAttributeBottom, across ? 0 : -after);

    [s->split addSubview:pane];
    [pane release]; /* retained by the split view */
    return pane;
}

static void min_size(NSView *pane, BOOL across, CGFloat min) {
    [pane addConstraint:[NSLayoutConstraint constraintWithItem:pane
                                                     attribute:across ? NSLayoutAttributeWidth : NSLayoutAttributeHeight
                                                     relatedBy:NSLayoutRelationGreaterThanOrEqual
                                                        toItem:nil attribute:NSLayoutAttributeNotAnAttribute
                                                    multiplier:1 constant:min]];
}

static uiControl *new_split(BOOL across, uiControl *first, uiControl *second,
                            int initial, int min_first, int min_second, const char *save_key) {
    SqlcSplit *s;
    uiDarwinNewControl(SqlcSplit, s);
    s->horzHuggingPri = NSLayoutPriorityDefaultLow;
    s->vertHuggingPri = NSLayoutPriorityDefaultLow;

    SqlcSplitView *sv = [[SqlcSplitView alloc] initWithFrame:NSZeroRect];
    [sv setVertical:across]; /* YES = side by side */
    [sv setDividerStyle:NSSplitViewDividerStyleThin];
    [sv setDelegate:sv];
    sv->minFirst = min_first;
    sv->minSecond = min_second;
    sv->wanted = initial;
    if (save_key) {
        sv->saveKey = [[NSString alloc] initWithUTF8String:save_key];
        double saved = [[NSUserDefaults standardUserDefaults] doubleForKey:sv->saveKey];
        if (saved >= min_first) sv->wanted = saved;
    }
    s->split = sv;

    // A little space on each side of the divider, like a padded uiBox.
    CGFloat gap = uiDarwinPaddingAmount(NULL) / 2;
    NSView *a = add_pane(s, 0, first, 0, gap);
    NSView *b = add_pane(s, 1, second, gap, 0);
    min_size(a, across, min_first);
    min_size(b, across, min_second);

    // When the window is resized, the first pane keeps its size and the
    // second one absorbs the change.
    [sv setHoldingPriority:NSLayoutPriorityDefaultLow + 10 forSubviewAtIndex:0];
    [sv setHoldingPriority:NSLayoutPriorityDefaultLow forSubviewAtIndex:1];

    return uiControl(s);
}

uiControl *sqlc_new_sidebar_split(uiControl *side, uiControl *main,
                                  int initial_width, int min_width,
                                  const char *save_key) {
    return new_split(YES, side, main, initial_width, min_width, MIN_MAIN_WIDTH, save_key);
}

uiControl *sqlc_new_stacked_split(uiControl *top, uiControl *bottom,
                                  int initial_height, int min_top, int min_bottom,
                                  const char *save_key) {
    return new_split(NO, top, bottom, initial_height, min_top, min_bottom, save_key);
}

int sqlc_split_position(uiControl *split) {
    SqlcSplitView *sv = SqlcSplit(split)->split;
    NSArray *panes = [sv arrangedSubviews];
    return [panes count] ? (int) [sv lengthOf:[[panes objectAtIndex:0] frame]] : 0;
}

void sqlc_split_set_position(uiControl *split, int size) {
    SqlcSplitView *sv = SqlcSplit(split)->split;
    [sv setPosition:[sv clampPosition:size] ofDividerAtIndex:0];
    [[sv window] layoutIfNeeded];
}

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
