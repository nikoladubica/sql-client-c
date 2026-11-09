// Loading indicator for libui-ng: a spinning NSProgressIndicator with a line
// of wrapping text below it, in one plain view, wrapped as a libui control.
// Used under the welcome screen's form while a connection is being opened.
#import <Cocoa/Cocoa.h>

#include "ui.h"
#include "ui_darwin.h"
#include "loading.h"

#define SqlcLoadingSignature 0x53716c64 /* "Sqld" */
#define SqlcLoading(this) ((SqlcLoading *) (this))

static const CGFloat SPINNER_SIZE = 32;
static const CGFloat GAP = 10;          /* between the spinner and the text */
static const CGFloat TEXT_HEIGHT = 48;  /* at least three lines of small text */

@interface SqlcLoadingView : NSView {
@public
    NSProgressIndicator *spinner; /* subviews: retained by the view hierarchy */
    NSTextField *text;
}
@end

@implementation SqlcLoadingView
// Wrap the text at the width we get, not at its one-line width.
- (void)layout {
    CGFloat w = NSWidth([self bounds]);
    if (w > 0 && [text preferredMaxLayoutWidth] != w) [text setPreferredMaxLayoutWidth:w];
    [super layout];
}
@end

typedef struct SqlcLoading {
    uiDarwinControl c;
    SqlcLoadingView *view;
} SqlcLoading;

// libui's default implementations ignore some of their parameters.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-parameter"
uiDarwinControlAllDefaultsExceptDestroy(SqlcLoading, view)
#pragma clang diagnostic pop

static void SqlcLoadingDestroy(uiControl *c) {
    SqlcLoading *l = SqlcLoading(c);
    [l->view->spinner stopAnimation:nil];
    [l->view release];
    uiFreeControl(c);
}

static void constrain(NSView *in, id a, NSLayoutAttribute aa, NSLayoutRelation rel,
                      id b, NSLayoutAttribute ba, CGFloat k) {
    [in addConstraint:[NSLayoutConstraint constraintWithItem:a attribute:aa relatedBy:rel
                                                      toItem:b attribute:ba
                                                  multiplier:1 constant:k]];
}

uiControl *sqlc_new_loading(void) {
    SqlcLoading *l;
    uiDarwinNewControl(SqlcLoading, l);

    SqlcLoadingView *v = [[SqlcLoadingView alloc] initWithFrame:NSZeroRect];

    NSProgressIndicator *spin = [[NSProgressIndicator alloc] initWithFrame:NSZeroRect];
    [spin setStyle:NSProgressIndicatorStyleSpinning];
    [spin setIndeterminate:YES];
    [spin setControlSize:NSControlSizeRegular];
    [spin setDisplayedWhenStopped:NO];
    [spin setTranslatesAutoresizingMaskIntoConstraints:NO];
    [v addSubview:spin];
    [spin release]; /* v keeps it */

    NSTextField *t = [NSTextField wrappingLabelWithString:@""]; /* autoreleased */
    [t setAlignment:NSTextAlignmentCenter];
    [t setFont:[NSFont systemFontOfSize:[NSFont smallSystemFontSize]]];
    [t setTextColor:[NSColor secondaryLabelColor]];
    [t setSelectable:NO];
    [t setTranslatesAutoresizingMaskIntoConstraints:NO];
    // Take the width we're given; never widen the form to fit a long line.
    [t setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow
                                forOrientation:NSLayoutConstraintOrientationHorizontal];
    [t setContentHuggingPriority:NSLayoutPriorityDefaultLow
                  forOrientation:NSLayoutConstraintOrientationHorizontal];
    [v addSubview:t];

    // spinner centred at the top, the text under it, full width, fixed height
    constrain(v, spin, NSLayoutAttributeCenterX, NSLayoutRelationEqual, v, NSLayoutAttributeCenterX, 0);
    constrain(v, spin, NSLayoutAttributeTop, NSLayoutRelationEqual, v, NSLayoutAttributeTop, 0);
    constrain(v, spin, NSLayoutAttributeWidth, NSLayoutRelationEqual, nil, NSLayoutAttributeNotAnAttribute, SPINNER_SIZE);
    constrain(v, spin, NSLayoutAttributeHeight, NSLayoutRelationEqual, nil, NSLayoutAttributeNotAnAttribute, SPINNER_SIZE);
    constrain(v, t, NSLayoutAttributeTop, NSLayoutRelationEqual, spin, NSLayoutAttributeBottom, GAP);
    constrain(v, t, NSLayoutAttributeLeading, NSLayoutRelationEqual, v, NSLayoutAttributeLeading, 0);
    constrain(v, t, NSLayoutAttributeTrailing, NSLayoutRelationEqual, v, NSLayoutAttributeTrailing, 0);
    // at least three lines (so the welcome form doesn't move), more in a
    // narrow place like the sidebar, where the text wraps further
    constrain(v, t, NSLayoutAttributeHeight, NSLayoutRelationGreaterThanOrEqual, nil, NSLayoutAttributeNotAnAttribute, TEXT_HEIGHT);
    [t setContentCompressionResistancePriority:NSLayoutPriorityRequired
                                forOrientation:NSLayoutConstraintOrientationVertical];
    constrain(v, t, NSLayoutAttributeBottom, NSLayoutRelationEqual, v, NSLayoutAttributeBottom, 0);
    constrain(v, v, NSLayoutAttributeWidth, NSLayoutRelationGreaterThanOrEqual, nil, NSLayoutAttributeNotAnAttribute, SPINNER_SIZE);

    v->spinner = spin;
    v->text = t;
    [t setHidden:YES];
    l->view = v;
    return uiControl(l);
}

void sqlc_loading_start(uiControl *c, const char *text) {
    SqlcLoadingView *v = SqlcLoading(c)->view;
    NSString *s = text ? [NSString stringWithUTF8String:text] : nil;
    [v->text setStringValue:s ? s : @""];
    [v->text setHidden:NO];
    [v->spinner startAnimation:nil];
}

void sqlc_loading_stop(uiControl *c) {
    SqlcLoadingView *v = SqlcLoading(c)->view;
    [v->spinner stopAnimation:nil];
    [v->text setStringValue:@""]; /* back to the minimum height */
    [v->text setHidden:YES];
}
