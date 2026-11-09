// In-place cell editing for a libui uiTable: a text field laid over the
// double-clicked cell, a calendar popover for date columns, and a cell
// context menu. See grid_edit.h.
#import <Cocoa/Cocoa.h>
#import <objc/runtime.h>

#include <stdlib.h>
#include <string.h>

#include "grid_edit.h"

static char editor_key; /* associated-object key on the NSTableView */

@interface SqlcGridEditor : NSObject <NSTextFieldDelegate, NSMenuDelegate> {
@public
    NSTableView *tv;          /* not retained: it owns us */
    grid_edit_callbacks cb;
    BOOL dead;                /* table is going away: no more callbacks */

    NSTextField *field;       /* open editor, or nil */
    NSInteger row, col;
    grid_editor_kind kind;
    BOOL finishing;
    NSPopover *popover;
    NSDatePicker *picker;

    NSInteger menuRow, menuCol;
}
@end

static NSDateFormatter *formatter(NSString *format) {
    NSDateFormatter *f = [[[NSDateFormatter alloc] init] autorelease];
    [f setLocale:[NSLocale localeWithLocaleIdentifier:@"en_US_POSIX"]];
    [f setTimeZone:[NSTimeZone timeZoneForSecondsFromGMT:0]];
    [f setDateFormat:format];
    return f;
}

/* What the user typed, as a date (or nil). */
static NSDate *parse_date(NSString *s) {
    static NSString *const formats[] = {
        @"yyyy-MM-dd HH:mm:ss", @"yyyy-MM-dd HH:mm", @"yyyy-MM-dd'T'HH:mm:ss",
        @"yyyy-MM-dd'T'HH:mm", @"yyyy-MM-dd", @"yyyy/MM/dd", nil };
    s = [s stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
    NSRange dot = [s rangeOfString:@"."];
    if (dot.location != NSNotFound) s = [s substringToIndex:dot.location]; /* drop fractions */
    for (int i = 0; formats[i]; i++) {
        NSDate *d = [formatter(formats[i]) dateFromString:s];
        if (d) return d;
    }
    return nil;
}

@implementation SqlcGridEditor

- (void)dealloc {
    [self closeQuietly];
    [super dealloc];
}

/* ---- clicks ---- */

- (void)onClicked:(id)sender {
    // libui's single-click handler; we became the table's target.
    if ([tv respondsToSelector:@selector(onClicked:)])
        [tv performSelector:@selector(onClicked:) withObject:sender];
}

- (void)doubleClicked:(id)sender {
    (void)sender;
    NSInteger r = [tv clickedRow], c = [tv clickedColumn];
    if (r < 0 || c < 0 || !cb.double_clicked) return;
    [self retain];
    dispatch_async(dispatch_get_main_queue(), ^{
        if (!dead) cb.double_clicked((int)r, (int)c, cb.data);
        [self release];
    });
}

/* ---- the editor ---- */

- (void)closeQuietly {
    if (popover) {
        [popover close];
        [popover release];
        popover = nil;
    }
    [picker release];
    picker = nil;
    if (field) {
        NSTextField *f = field;
        field = nil;
        [f setDelegate:nil];
        BOOL had_focus = [[f window] firstResponder] == [f currentEditor];
        [f abortEditing];
        [f removeFromSuperview];
        if (had_focus) [[tv window] makeFirstResponder:tv];
        [f release];
    }
}

- (void)showCalendar {
    picker = [[NSDatePicker alloc] initWithFrame:NSZeroRect];
    [picker setDatePickerStyle:NSDatePickerStyleClockAndCalendar];
    [picker setDatePickerElements:kind == GRID_EDITOR_DATE
        ? NSDatePickerElementFlagYearMonthDay
        : NSDatePickerElementFlagYearMonthDay | NSDatePickerElementFlagHourMinuteSecond];
    [picker setBezeled:NO];
    [picker setDrawsBackground:NO];
    NSCalendar *cal = [[[NSCalendar alloc] initWithCalendarIdentifier:NSCalendarIdentifierGregorian] autorelease];
    [cal setTimeZone:[NSTimeZone timeZoneForSecondsFromGMT:0]];
    [picker setCalendar:cal];
    [picker setTimeZone:[NSTimeZone timeZoneForSecondsFromGMT:0]];
    NSDate *d = parse_date([field stringValue]);
    if (!d) { /* nothing typed: start at today (local date, as GMT) */
        NSDate *now = [NSDate date];
        d = [now dateByAddingTimeInterval:[[NSTimeZone localTimeZone] secondsFromGMTForDate:now]];
    }
    [picker setDateValue:d];
    [picker setTarget:self];
    [picker setAction:@selector(pickerChanged:)];
    [picker sizeToFit];

    NSSize sz = [picker frame].size;
    NSView *box = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, sz.width + 16, sz.height + 16)];
    [picker setFrameOrigin:NSMakePoint(8, 8)];
    [box addSubview:picker];
    NSViewController *vc = [[NSViewController alloc] init];
    [vc setView:box];
    [box release];

    popover = [[NSPopover alloc] init];
    [popover setContentViewController:vc];
    [vc release];
    [popover setBehavior:NSPopoverBehaviorApplicationDefined];
    [popover setAnimates:NO];
    [popover showRelativeToRect:[field bounds] ofView:field
                  preferredEdge:[field isFlipped] ? NSRectEdgeMaxY : NSRectEdgeMinY];
    [[tv window] makeKeyWindow]; /* keep typing in the field */
}

- (void)pickerChanged:(id)sender {
    (void)sender;
    if (!field || !picker) return;
    NSString *fmt = kind == GRID_EDITOR_DATE ? @"yyyy-MM-dd" : @"yyyy-MM-dd HH:mm:ss";
    [field setStringValue:[formatter(fmt) stringFromDate:[picker dateValue]]];
    // Clicking the calendar made the popover key: give the keyboard back to
    // the field, so Return saves and Escape cancels.
    [[tv window] makeKeyWindow];
    if (![field currentEditor]) { /* lost focus somehow: take it back */
        finishing = YES;
        [field selectText:nil];
        finishing = NO;
    }
    NSText *ed = [field currentEditor];
    [ed setSelectedRange:NSMakeRange([[ed string] length], 0)];
}

- (void)controlTextDidChange:(NSNotification *)n {
    (void)n;
    if (!picker) return;
    NSDate *d = parse_date([field stringValue]);
    if (d) [picker setDateValue:d];
}

- (void)beginRow:(NSInteger)r col:(NSInteger)c text:(const char *)text
     placeholder:(const char *)placeholder kind:(grid_editor_kind)k {
    [self closeQuietly];
    if (r < 0 || c < 0 || r >= [tv numberOfRows] || c >= [tv numberOfColumns]) return;
    row = r;
    col = c;
    kind = k;
    [tv scrollRowToVisible:r];
    [tv scrollColumnToVisible:c];

    NSRect frame = [tv frameOfCellAtColumn:c row:r];
    if (frame.size.width < 140) frame.size.width = 140;
    if (frame.size.height < 22) {
        frame.origin.y -= (22 - frame.size.height) / 2;
        frame.size.height = 22;
    }
    field = [[NSTextField alloc] initWithFrame:frame];
    [field setStringValue:[NSString stringWithUTF8String:text ? text : ""] ?: @""];
    if (placeholder) [field setPlaceholderString:[NSString stringWithUTF8String:placeholder]];
    [field setFont:[NSFont systemFontOfSize:[NSFont systemFontSize]]];
    [field setBezeled:YES];
    [field setEditable:YES];
    [field setDelegate:self];
    [[field cell] setScrollable:YES];
    [[field cell] setUsesSingleLineMode:YES];
    [tv addSubview:field];
    // selectText: focuses the field and selects its text. (Focusing it first
    // with makeFirstResponder: would have selectText: end that session.)
    // Any end-of-editing from whatever had focus before isn't ours.
    finishing = YES;
    [field selectText:nil];
    if (k != GRID_EDITOR_TEXT) [self showCalendar];
    finishing = NO;
}

/* End the edit and report the text. */
- (void)finish:(grid_end)how {
    if (!field || finishing) return;
    finishing = YES;
    const char *u = [[field stringValue] UTF8String];
    char *text = strdup(u ? u : "");
    NSInteger r = row, c = col;
    [self closeQuietly];
    finishing = NO;
    if (!text) return;
    [self retain];
    dispatch_async(dispatch_get_main_queue(), ^{
        if (!dead && cb.committed) cb.committed((int)r, (int)c, text, how, cb.data);
        free(text);
        [self release];
    });
}

- (void)cancel {
    if (!field) return;
    NSInteger r = row, c = col;
    [self closeQuietly];
    [self retain];
    dispatch_async(dispatch_get_main_queue(), ^{
        if (!dead && cb.cancelled) cb.cancelled((int)r, (int)c, cb.data);
        [self release];
    });
}

- (BOOL)control:(NSControl *)control textView:(NSTextView *)textView doCommandBySelector:(SEL)sel {
    (void)control; (void)textView;
    if (sel == @selector(insertNewline:)) { [self finish:GRID_END_RETURN]; return YES; }
    if (sel == @selector(insertTab:)) { [self finish:GRID_END_TAB]; return YES; }
    if (sel == @selector(insertBacktab:)) { [self finish:GRID_END_BACKTAB]; return YES; }
    if (sel == @selector(cancelOperation:)) { [self cancel]; return YES; }
    return NO;
}

- (void)controlTextDidEndEditing:(NSNotification *)n {
    (void)n;
    if (field && !finishing) [self finish:GRID_END_BLUR]; /* clicked elsewhere */
}

/* ---- context menu ---- */

- (void)menuNeedsUpdate:(NSMenu *)menu {
    [menu removeAllItems];
    NSInteger r = [tv clickedRow], c = [tv clickedColumn];
    menuRow = r;
    menuCol = c;
    if (r < 0 || r >= [tv numberOfRows]) return;
    // Keep a multi-row selection that includes the row; otherwise select it.
    if (![[tv selectedRowIndexes] containsIndex:(NSUInteger)r])
        [tv selectRowIndexes:[NSIndexSet indexSetWithIndex:(NSUInteger)r] byExtendingSelection:NO];
    unsigned enabled = cb.menu_open ? cb.menu_open((int)r, (int)c, cb.data) : 0;
    if (enabled == 0) return;
    NSUInteger nsel = [[tv selectedRowIndexes] count];
    NSString *del = nsel > 1 ? [NSString stringWithFormat:@"Delete %lu Rows…", (unsigned long)nsel]
                             : @"Delete Row…";
    NSString *titles[GRID_MENU_COUNT] = { @"Edit Cell", @"Set to NULL", @"Copy Value", del };
    for (int i = 0; i < GRID_MENU_COUNT; i++) {
        if (i == GRID_MENU_COPY || i == GRID_MENU_DELETE_ROWS) [menu addItem:[NSMenuItem separatorItem]];
        NSMenuItem *item = [[NSMenuItem alloc] initWithTitle:titles[i] action:@selector(chosen:) keyEquivalent:@""];
        [item setTag:i];
        [item setTarget:self];
        [item setEnabled:(enabled & GRID_MENU_BIT(i)) != 0];
        [menu addItem:item];
        [item release];
    }
}

- (void)chosen:(NSMenuItem *)item {
    grid_menu_item which = (grid_menu_item)[item tag];
    NSInteger r = menuRow, c = menuCol;
    [self retain];
    dispatch_async(dispatch_get_main_queue(), ^{
        if (!dead && cb.menu_action) cb.menu_action(which, (int)r, (int)c, cb.data);
        [self release];
    });
}
@end

/* ---------- C API ---------- */

static NSTableView *table_view(uiTable *t) {
    NSScrollView *sv = (NSScrollView *) uiControlHandle(uiControl(t));
    id doc = [sv documentView];
    return [doc isKindOfClass:[NSTableView class]] ? doc : nil;
}

static SqlcGridEditor *editor_of(uiTable *t) {
    NSTableView *tv = table_view(t);
    return tv ? objc_getAssociatedObject(tv, &editor_key) : nil;
}

void sqlc_grid_enable_editing(uiTable *t, const grid_edit_callbacks *cb) {
    NSTableView *tv = table_view(t);
    if (!tv) return;
    SqlcGridEditor *e = [SqlcGridEditor new];
    e->tv = tv;
    e->cb = *cb;
    e->menuRow = e->menuCol = -1;
    [tv setTarget:e];
    [tv setAction:@selector(onClicked:)];
    [tv setDoubleAction:@selector(doubleClicked:)];
    NSMenu *menu = [[NSMenu alloc] initWithTitle:@"Cell"];
    [menu setAutoenablesItems:NO];
    [menu setDelegate:e];
    [tv setMenu:menu];
    [menu release];
    objc_setAssociatedObject(tv, &editor_key, e, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    [e release];
}

void sqlc_grid_begin_edit(uiTable *t, int row, int col, const char *text,
                          const char *placeholder, grid_editor_kind kind) {
    SqlcGridEditor *e = editor_of(t);
    if (e && !e->dead) [e beginRow:row col:col text:text placeholder:placeholder kind:kind];
}

void sqlc_grid_close_editor(uiTable *t) {
    SqlcGridEditor *e = editor_of(t);
    if (e) [e closeQuietly];
}

void sqlc_grid_detach(uiTable *t) {
    SqlcGridEditor *e = editor_of(t);
    if (!e) return;
    e->dead = YES;          /* drop callbacks already queued ... */
    [e closeQuietly];
    [[e->tv menu] cancelTrackingWithoutAnimation];
}

int sqlc_grid_editing(uiTable *t, int *row, int *col, char **text) {
    SqlcGridEditor *e = editor_of(t);
    if (!e || !e->field) return 0;
    if (row) *row = (int)e->row;
    if (col) *col = (int)e->col;
    if (text) {
        const char *u = [[e->field stringValue] UTF8String];
        *text = strdup(u ? u : "");
    }
    return 1;
}

int sqlc_grid_editor_has_calendar(uiTable *t) {
    SqlcGridEditor *e = editor_of(t);
    return e && e->field && e->picker != nil;
}

void sqlc_grid_scroll_to_row(uiTable *t, int row) {
    NSTableView *tv = table_view(t);
    if (tv && row >= 0 && row < [tv numberOfRows]) [tv scrollRowToVisible:row];
}

void sqlc_grid_test_finish(uiTable *t, const char *text, grid_end how) {
    SqlcGridEditor *e = editor_of(t);
    if (!e || !e->field) return;
    [e->field setStringValue:[NSString stringWithUTF8String:text] ?: @""];
    [e finish:how];
}

void sqlc_grid_test_pick_date(uiTable *t, const char *value) {
    SqlcGridEditor *e = editor_of(t);
    NSDate *d = e && e->picker ? parse_date([NSString stringWithUTF8String:value]) : nil;
    if (!d) return;
    [e->picker setDateValue:d];
    [e pickerChanged:e->picker];
}
