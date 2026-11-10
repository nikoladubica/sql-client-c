// The window's tab strip for libui-ng: one custom NSView wrapped as a libui
// control, drawing left-aligned tabs (title + close button), a "+" button
// after the last one and a separator below. libui-ng's uiTab (NSTabView)
// centres its tabs and can't host close or "+" buttons. See tabbar.h.
#import <Cocoa/Cocoa.h>

#include "ui.h"
#include "ui_darwin.h"
#include "tabbar.h"

#define SqlcTabBarSignature 0x53717462 /* "Sqtb" */
#define SqlcTabBar(this) ((SqlcTabBar *) (this))

#define BAR_HEIGHT 30.0
#define ITEM_MAX 220.0  /* widest a tab gets */
#define ITEM_MIN 100.0  /* narrowest; beyond that the strip clips */
#define PLUS_WIDTH 30.0
#define CLOSE_SIZE 16.0
#define PAD 10.0

typedef struct SqlcTabBar SqlcTabBar;

/* What a mouse press is on. */
typedef enum { HIT_NONE, HIT_ITEM, HIT_CLOSE, HIT_PLUS } hit_kind;

@interface SqlcTabBarView : NSView {
@public
    SqlcTabBar *owner;       /* not retained: the plain C struct that owns us */
    NSMutableArray *titles;  /* NSString per tab (owned) */
    NSInteger selected;      /* -1 = none */
    hit_kind pressKind;      /* close / plus pressed, waiting for mouse up */
    NSInteger pressIndex;
    BOOL pressInside;        /* the pointer is still over it (highlight) */
    NSInteger middleIndex;   /* tab a middle click started on, or -1 */
}
@end

struct SqlcTabBar {
    uiDarwinControl c;
    SqlcTabBarView *view;
    sqlc_tabbar_callbacks cb;
};

@implementation SqlcTabBarView

- (instancetype)initWithFrame:(NSRect)frame {
    self = [super initWithFrame:frame];
    if (self) {
        titles = [[NSMutableArray alloc] init];
        selected = -1;
        pressKind = HIT_NONE;
        pressIndex = -1;
        middleIndex = -1;
    }
    return self;
}

- (void)dealloc {
    [titles release];
    [super dealloc];
}

- (BOOL)isFlipped { return YES; }
- (BOOL)mouseDownCanMoveWindow { return NO; }
- (BOOL)acceptsFirstMouse:(NSEvent *)event { (void)event; return YES; }

- (NSSize)intrinsicContentSize {
    return NSMakeSize(NSViewNoIntrinsicMetric, BAR_HEIGHT);
}

/* Width of each tab: they shrink as tabs are added, between the limits. */
- (CGFloat)itemWidth {
    NSUInteger n = [titles count];
    if (n == 0) return ITEM_MAX;
    CGFloat w = floor(([self bounds].size.width - PLUS_WIDTH) / (CGFloat)n);
    if (w > ITEM_MAX) w = ITEM_MAX;
    if (w < ITEM_MIN) w = ITEM_MIN;
    return w;
}

- (NSRect)itemRect:(NSInteger)i {
    CGFloat w = [self itemWidth];
    return NSMakeRect(w * (CGFloat)i, 0, w, BAR_HEIGHT - 1);
}

- (NSRect)closeRect:(NSInteger)i {
    NSRect r = [self itemRect:i];
    return NSMakeRect(NSMaxX(r) - 8 - CLOSE_SIZE, floor((r.size.height - CLOSE_SIZE) / 2),
                      CLOSE_SIZE, CLOSE_SIZE);
}

- (NSRect)plusRect {
    CGFloat x = [self itemWidth] * (CGFloat)[titles count];
    return NSMakeRect(x, 0, PLUS_WIDTH, BAR_HEIGHT - 1);
}

/* What point p is on; *index is the tab for HIT_ITEM / HIT_CLOSE. */
- (hit_kind)hitAt:(NSPoint)p index:(NSInteger *)index {
    *index = -1;
    if (NSPointInRect(p, [self plusRect])) return HIT_PLUS;
    NSInteger n = (NSInteger)[titles count];
    for (NSInteger i = 0; i < n; i++) {
        if (!NSPointInRect(p, [self itemRect:i])) continue;
        *index = i;
        return NSPointInRect(p, NSInsetRect([self closeRect:i], -2, -2)) ? HIT_CLOSE : HIT_ITEM;
    }
    return HIT_NONE;
}

/* ---- tooltips: the full title of each tab ---- */

- (NSString *)view:(NSView *)view stringForToolTip:(NSToolTipTag)tag point:(NSPoint)point
          userData:(void *)data {
    (void)view; (void)tag; (void)point;
    NSInteger i = (NSInteger)(intptr_t)data;
    if (i < 0 || i >= (NSInteger)[titles count]) return @"";
    return [titles objectAtIndex:(NSUInteger)i];
}

- (void)rebuildToolTips {
    [self removeAllToolTips];
    NSInteger n = (NSInteger)[titles count];
    for (NSInteger i = 0; i < n; i++)
        [self addToolTipRect:[self itemRect:i] owner:self userData:(void *)(intptr_t)i];
}

- (void)setFrameSize:(NSSize)size {
    [super setFrameSize:size];
    [self rebuildToolTips];
}

- (void)changed {
    pressKind = HIT_NONE;
    pressIndex = -1;
    middleIndex = -1;
    [self rebuildToolTips];
    [self setNeedsDisplay:YES];
}

/* ---- drawing ---- */

static void draw_cross(NSRect r, CGFloat half, NSColor *color) {
    NSPoint c = NSMakePoint(NSMidX(r), NSMidY(r));
    NSBezierPath *p = [NSBezierPath bezierPath]; // autoreleased
    [p moveToPoint:NSMakePoint(c.x - half, c.y - half)];
    [p lineToPoint:NSMakePoint(c.x + half, c.y + half)];
    [p moveToPoint:NSMakePoint(c.x + half, c.y - half)];
    [p lineToPoint:NSMakePoint(c.x - half, c.y + half)];
    [p setLineWidth:1.3];
    [p setLineCapStyle:NSLineCapStyleRound];
    [color setStroke];
    [p stroke];
}

static void draw_plus(NSRect r, CGFloat half, NSColor *color) {
    NSPoint c = NSMakePoint(NSMidX(r), NSMidY(r));
    NSBezierPath *p = [NSBezierPath bezierPath]; // autoreleased
    [p moveToPoint:NSMakePoint(c.x - half, c.y)];
    [p lineToPoint:NSMakePoint(c.x + half, c.y)];
    [p moveToPoint:NSMakePoint(c.x, c.y - half)];
    [p lineToPoint:NSMakePoint(c.x, c.y + half)];
    [p setLineWidth:1.5];
    [p setLineCapStyle:NSLineCapStyleRound];
    [color setStroke];
    [p stroke];
}

static void fill_pressed(NSRect r) {
    [[[NSColor labelColor] colorWithAlphaComponent:0.15] setFill];
    [[NSBezierPath bezierPathWithRoundedRect:r xRadius:4 yRadius:4] fill];
}

- (void)drawRect:(NSRect)dirty {
    (void)dirty;
    NSRect b = [self bounds];
    NSInteger n = (NSInteger)[titles count];

    NSMutableParagraphStyle *para = [[NSMutableParagraphStyle alloc] init];
    [para setLineBreakMode:NSLineBreakByTruncatingTail];
    NSFont *font = [NSFont systemFontOfSize:12];
    NSFont *bold = [NSFont boldSystemFontOfSize:12];

    for (NSInteger i = 0; i < n; i++) {
        NSRect r = [self itemRect:i];
        BOOL sel = i == selected;
        if (sel) {
            [[NSColor controlBackgroundColor] setFill];
            NSRectFill(r);
            [[NSColor controlAccentColor] setFill];
            NSRectFill(NSMakeRect(r.origin.x, NSMaxY(r) - 2, r.size.width, 2));
        }
        [[NSColor separatorColor] setFill];
        NSRectFill(NSMakeRect(NSMaxX(r) - 1, r.origin.y + 6, 1, r.size.height - 12));

        NSRect close = [self closeRect:i];
        if (pressKind == HIT_CLOSE && pressIndex == i && pressInside) fill_pressed(close);
        draw_cross(close, 3.5, sel ? [NSColor labelColor] : [NSColor secondaryLabelColor]);

        NSDictionary *attrs = @{
            NSFontAttributeName: sel ? bold : font,
            NSForegroundColorAttributeName: sel ? [NSColor labelColor] : [NSColor secondaryLabelColor],
            NSParagraphStyleAttributeName: para,
        };
        NSString *title = [titles objectAtIndex:(NSUInteger)i];
        CGFloat lineH = ceil([font ascender] - [font descender]);
        NSRect tr = NSMakeRect(r.origin.x + PAD, floor((r.size.height - lineH) / 2),
                               NSMinX(close) - 4 - (r.origin.x + PAD), lineH);
        if (tr.size.width > 0) [title drawInRect:tr withAttributes:attrs];
    }

    NSRect plus = [self plusRect];
    if (pressKind == HIT_PLUS && pressInside) fill_pressed(NSInsetRect(plus, 3, 4));
    draw_plus(plus, 5, [NSColor secondaryLabelColor]);

    [[NSColor separatorColor] setFill];
    NSRectFill(NSMakeRect(0, b.size.height - 1, b.size.width, 1));
    [para release];
}

/* ---- mouse ---- */

- (void)mouseDown:(NSEvent *)event {
    NSPoint p = [self convertPoint:[event locationInWindow] fromView:nil];
    NSInteger i;
    hit_kind k = [self hitAt:p index:&i];
    if (k == HIT_ITEM) {
        if (i == selected) return;
        selected = i;
        [self setNeedsDisplay:YES];
        SqlcTabBar *o = owner;
        if (o && o->cb.on_select) o->cb.on_select(uiControl(o), (int)i, o->cb.data); /* last */
        return;
    }
    if (k == HIT_CLOSE || k == HIT_PLUS) {
        pressKind = k;
        pressIndex = i;
        pressInside = YES;
        [self setNeedsDisplay:YES];
    }
}

- (void)mouseDragged:(NSEvent *)event {
    if (pressKind == HIT_NONE) return;
    NSPoint p = [self convertPoint:[event locationInWindow] fromView:nil];
    NSInteger i;
    hit_kind k = [self hitAt:p index:&i];
    BOOL inside = k == pressKind && i == pressIndex;
    if (inside != pressInside) {
        pressInside = inside;
        [self setNeedsDisplay:YES];
    }
}

- (void)mouseUp:(NSEvent *)event {
    if (pressKind == HIT_NONE) return;
    NSPoint p = [self convertPoint:[event locationInWindow] fromView:nil];
    NSInteger i;
    hit_kind k = [self hitAt:p index:&i];
    hit_kind pressed = pressKind;
    NSInteger index = pressIndex;
    pressKind = HIT_NONE;
    pressIndex = -1;
    [self setNeedsDisplay:YES];
    if (k != pressed || i != index) return; /* released elsewhere: nothing */
    SqlcTabBar *o = owner;
    if (!o) return;
    /* last: the callback may change the strip */
    if (pressed == HIT_CLOSE && o->cb.on_close) o->cb.on_close(uiControl(o), (int)index, o->cb.data);
    else if (pressed == HIT_PLUS && o->cb.on_new) o->cb.on_new(uiControl(o), o->cb.data);
}

- (void)otherMouseDown:(NSEvent *)event {
    middleIndex = -1;
    if ([event buttonNumber] != 2) { [super otherMouseDown:event]; return; }
    NSPoint p = [self convertPoint:[event locationInWindow] fromView:nil];
    NSInteger i;
    hit_kind k = [self hitAt:p index:&i];
    if (k == HIT_ITEM || k == HIT_CLOSE) middleIndex = i;
}

- (void)otherMouseUp:(NSEvent *)event {
    if ([event buttonNumber] != 2) { [super otherMouseUp:event]; return; }
    NSPoint p = [self convertPoint:[event locationInWindow] fromView:nil];
    NSInteger i;
    hit_kind k = [self hitAt:p index:&i];
    NSInteger started = middleIndex;
    middleIndex = -1;
    if ((k != HIT_ITEM && k != HIT_CLOSE) || i != started) return;
    SqlcTabBar *o = owner;
    if (o && o->cb.on_close) o->cb.on_close(uiControl(o), (int)i, o->cb.data); /* last */
}

@end

// libui's default implementations ignore some of their parameters.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-parameter"
uiDarwinControlAllDefaultsExceptDestroy(SqlcTabBar, view)
#pragma clang diagnostic pop

static void SqlcTabBarDestroy(uiControl *c) {
    SqlcTabBar *s = SqlcTabBar(c);
    s->view->owner = NULL;
    [s->view removeAllToolTips];
    [s->view release];
    uiFreeControl(c);
}

uiControl *sqlc_new_tabbar(const sqlc_tabbar_callbacks *cb) {
    SqlcTabBar *s;
    uiDarwinNewControl(SqlcTabBar, s);
    if (cb) s->cb = *cb;
    SqlcTabBarView *v = [[SqlcTabBarView alloc] initWithFrame:NSZeroRect];
    v->owner = s;
    s->view = v;
    return uiControl(s);
}

static NSString *ns_title(const char *title) {
    NSString *s = [NSString stringWithUTF8String:title ? title : ""]; // autoreleased
    return s ? s : @"";
}

int sqlc_tabbar_append(uiControl *bar, const char *title) {
    SqlcTabBarView *v = SqlcTabBar(bar)->view;
    [v->titles addObject:ns_title(title)];
    if (v->selected < 0) v->selected = 0;
    [v changed];
    return (int)[v->titles count] - 1;
}

void sqlc_tabbar_remove(uiControl *bar, int index) {
    SqlcTabBarView *v = SqlcTabBar(bar)->view;
    NSInteger n = (NSInteger)[v->titles count];
    if (index < 0 || index >= n) return;
    [v->titles removeObjectAtIndex:(NSUInteger)index];
    n--;
    if (n == 0) v->selected = -1;
    else if (index < v->selected) v->selected--;
    else if (v->selected >= n) v->selected = n - 1;
    [v changed];
}

void sqlc_tabbar_set_title(uiControl *bar, int index, const char *title) {
    SqlcTabBarView *v = SqlcTabBar(bar)->view;
    if (index < 0 || index >= (NSInteger)[v->titles count]) return;
    [v->titles replaceObjectAtIndex:(NSUInteger)index withObject:ns_title(title)];
    [v changed];
}

void sqlc_tabbar_set_selected(uiControl *bar, int index) {
    SqlcTabBarView *v = SqlcTabBar(bar)->view;
    if (index < 0 || index >= (NSInteger)[v->titles count]) return;
    v->selected = index;
    [v setNeedsDisplay:YES];
}

int sqlc_tabbar_selected(uiControl *bar) {
    return (int)SqlcTabBar(bar)->view->selected;
}

int sqlc_tabbar_count(uiControl *bar) {
    return (int)[SqlcTabBar(bar)->view->titles count];
}
