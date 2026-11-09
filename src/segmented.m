// Segmented switch for libui-ng: an NSSegmentedControl wrapped as a libui
// control (libui-ng has none). Used for the main area's
// Normal | AI | Structure switch.
#import <Cocoa/Cocoa.h>

#include "ui.h"
#include "ui_darwin.h"
#include "segmented.h"

#define SqlcSegmentedSignature 0x53717367 /* "Sqsg" */
#define SqlcSegmented(this) ((SqlcSegmented *) (this))

typedef struct SqlcSegmented SqlcSegmented;

@interface SqlcSegmentedControl : NSSegmentedControl {
@public
    SqlcSegmented *owner; /* not retained: a plain C struct that owns us */
}
- (void)changed:(id)sender;
@end

struct SqlcSegmented {
    uiDarwinControl c;
    SqlcSegmentedControl *seg;
    sqlc_segmented_fn onChanged;
    void *onChangedData;
};

@implementation SqlcSegmentedControl
- (void)changed:(id)sender {
    (void)sender;
    SqlcSegmented *s = owner;
    if (s && s->onChanged) s->onChanged(uiControl(s), (int) [self selectedSegment], s->onChangedData);
}
@end

// libui's default implementations ignore some of their parameters.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-parameter"
uiDarwinControlAllDefaultsExceptDestroy(SqlcSegmented, seg)
#pragma clang diagnostic pop

static void SqlcSegmentedDestroy(uiControl *c) {
    SqlcSegmented *s = SqlcSegmented(c);
    s->seg->owner = NULL;
    [s->seg setTarget:nil];
    [s->seg release];
    uiFreeControl(c);
}

uiControl *sqlc_new_segmented(const char *const *labels, int n) {
    if (!labels || n < 1) return NULL;
    SqlcSegmented *s;
    uiDarwinNewControl(SqlcSegmented, s);

    SqlcSegmentedControl *seg = [[SqlcSegmentedControl alloc] initWithFrame:NSZeroRect];
    seg->owner = s;
    [seg setSegmentCount:n];
    for (int i = 0; i < n; i++) {
        NSString *title = [NSString stringWithUTF8String:labels[i] ? labels[i] : ""];
        [seg setLabel:(title ? title : @"") forSegment:i];
    }
    [seg setTrackingMode:NSSegmentSwitchTrackingSelectOne];
    [seg setSegmentStyle:NSSegmentStyleRounded];
    [seg setControlSize:NSControlSizeSmall];
    [seg setFont:[NSFont systemFontOfSize:[NSFont systemFontSizeForControlSize:NSControlSizeSmall]]];
    [seg setSelectedSegment:0];
    [seg setTarget:seg];
    [seg setAction:@selector(changed:)];
    s->seg = seg;
    return uiControl(s);
}

int sqlc_segmented_selected(uiControl *seg) {
    return (int) [SqlcSegmented(seg)->seg selectedSegment];
}

void sqlc_segmented_set_selected(uiControl *seg, int index) {
    SqlcSegmentedControl *sc = SqlcSegmented(seg)->seg;
    if (index < 0 || index >= [sc segmentCount]) return;
    [sc setSelectedSegment:index]; /* sends no action */
}

void sqlc_segmented_on_changed(uiControl *seg, sqlc_segmented_fn f, void *data) {
    SqlcSegmented *s = SqlcSegmented(seg);
    s->onChanged = f;
    s->onChangedData = data;
}
