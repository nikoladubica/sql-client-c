// A resizable sidebar for libui-ng: an NSSplitView wrapped as a libui
// container control (libui-ng has no splitter of its own).
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
    CGFloat wantedWidth; /* sidebar width to apply on the first layout */
    CGFloat minWidth;
    NSString *saveKey;   /* retained; nil = don't persist */
    BOOL positioned;     /* initial width applied; saving is allowed */
}
@end

@implementation SqlcSplitView
- (void)dealloc {
    [saveKey release];
    [super dealloc];
}

- (void)layout {
    [super layout];
    // Panes have no size until the first layout pass, so apply the starting
    // width here, once.
    if (!positioned && NSWidth([self bounds]) > 0) {
        CGFloat w = wantedWidth;
        CGFloat max = NSWidth([self bounds]) - MIN_MAIN_WIDTH;
        if (w > max) w = max;
        if (w < minWidth) w = minWidth;
        [self setPosition:w ofDividerAtIndex:0];
        positioned = YES;
    }
}

// Constraints already enforce these; the delegate keeps the drag itself from
// overshooting so the divider doesn't jump back.
- (CGFloat)splitView:(NSSplitView *)sv constrainMinCoordinate:(CGFloat)proposed ofSubviewAt:(NSInteger)i {
    (void)sv; (void)i;
    return proposed < minWidth ? minWidth : proposed;
}

- (CGFloat)splitView:(NSSplitView *)sv constrainMaxCoordinate:(CGFloat)proposed ofSubviewAt:(NSInteger)i {
    (void)i;
    CGFloat max = NSWidth([sv bounds]) - MIN_MAIN_WIDTH - [sv dividerThickness];
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
    CGFloat w = NSWidth([[panes objectAtIndex:0] frame]);
    if (w >= minWidth)
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

/* Put child in a plain pane view that fills it; the split view sizes the pane. */
static NSView *add_pane(SqlcSplit *s, int i, uiControl *child, CGFloat lead, CGFloat trail) {
    NSView *pane = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 100, 100)];
    s->children[i] = child;
    uiControlSetParent(child, uiControl(s));
    uiDarwinControlSetSuperview(uiDarwinControl(child), pane);
    uiDarwinControlSyncEnableState(uiDarwinControl(child), uiControlEnabledToUser(uiControl(s)));
    // Fill the pane rather than hugging the child's content.
    uiDarwinControlSetHuggingPriority(uiDarwinControl(child), NSLayoutPriorityDefaultLow, NSLayoutConstraintOrientationHorizontal);
    uiDarwinControlSetHuggingPriority(uiDarwinControl(child), NSLayoutPriorityDefaultLow, NSLayoutConstraintOrientationVertical);

    NSView *cv = (NSView *) uiControlHandle(child);
    pin(pane, cv, NSLayoutAttributeLeading, lead);
    pin(pane, cv, NSLayoutAttributeTrailing, -trail);
    pin(pane, cv, NSLayoutAttributeTop, 0);
    pin(pane, cv, NSLayoutAttributeBottom, 0);

    [s->split addSubview:pane];
    [pane release]; /* retained by the split view */
    return pane;
}

uiControl *sqlc_new_sidebar_split(uiControl *side, uiControl *main,
                                  int initial_width, int min_width,
                                  const char *save_key) {
    SqlcSplit *s;
    uiDarwinNewControl(SqlcSplit, s);
    s->horzHuggingPri = NSLayoutPriorityDefaultLow;
    s->vertHuggingPri = NSLayoutPriorityDefaultLow;

    SqlcSplitView *sv = [[SqlcSplitView alloc] initWithFrame:NSZeroRect];
    [sv setVertical:YES]; /* side by side */
    [sv setDividerStyle:NSSplitViewDividerStyleThin];
    [sv setDelegate:sv];
    sv->minWidth = min_width;
    sv->wantedWidth = initial_width;
    if (save_key) {
        sv->saveKey = [[NSString alloc] initWithUTF8String:save_key];
        double saved = [[NSUserDefaults standardUserDefaults] doubleForKey:sv->saveKey];
        if (saved >= min_width) sv->wantedWidth = saved;
    }
    s->split = sv;

    // A little space on each side of the divider, like a padded uiBox.
    CGFloat gap = uiDarwinPaddingAmount(NULL) / 2;
    NSView *left = add_pane(s, 0, side, 0, gap);
    NSView *right = add_pane(s, 1, main, gap, 0);

    [left addConstraint:[NSLayoutConstraint constraintWithItem:left attribute:NSLayoutAttributeWidth
                                                     relatedBy:NSLayoutRelationGreaterThanOrEqual
                                                        toItem:nil attribute:NSLayoutAttributeNotAnAttribute
                                                    multiplier:1 constant:min_width]];
    [right addConstraint:[NSLayoutConstraint constraintWithItem:right attribute:NSLayoutAttributeWidth
                                                      relatedBy:NSLayoutRelationGreaterThanOrEqual
                                                         toItem:nil attribute:NSLayoutAttributeNotAnAttribute
                                                     multiplier:1 constant:MIN_MAIN_WIDTH]];

    // When the window is resized, the sidebar keeps its width and the main
    // pane absorbs the change.
    [sv setHoldingPriority:NSLayoutPriorityDefaultLow + 10 forSubviewAtIndex:0];
    [sv setHoldingPriority:NSLayoutPriorityDefaultLow forSubviewAtIndex:1];

    return uiControl(s);
}

int sqlc_split_sidebar_width(uiControl *split) {
    NSArray *panes = [SqlcSplit(split)->split arrangedSubviews];
    return [panes count] ? (int) NSWidth([[panes objectAtIndex:0] frame]) : 0;
}

void sqlc_split_set_sidebar_width(uiControl *split, int width) {
    SqlcSplitView *sv = SqlcSplit(split)->split;
    [sv setPosition:width ofDividerAtIndex:0];
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
