// Right-click menu for a libui uiTable (libui-ng has no context-menu API),
// plus the small native dialogs its actions need.
#import <Cocoa/Cocoa.h>
#import <objc/runtime.h>

#include <stdio.h>
#include <string.h>

#include "table_menu.h"

static NSString *const titles[TABLE_MENU_COUNT] = {
    @"Copy Table Name",
    @"Rename Table…",
    @"Truncate Table…",
    @"Delete Table…",
    @"Export…",
};

static char menu_key; /* associated-object key on the NSTableView */

@interface SqlcTableMenu : NSObject <NSMenuDelegate> {
@public
    NSTableView *tv;  /* not retained: it owns us */
    table_menu_open_fn onOpen;
    table_menu_action_fn onAction;
    void *data;
    NSInteger row;    /* row the open menu is for */
}
- (void)fillForRow:(NSInteger)r menu:(NSMenu *)menu;
- (void)chosen:(NSMenuItem *)item;
@end

@implementation SqlcTableMenu
- (void)fillForRow:(NSInteger)r menu:(NSMenu *)menu {
    [menu removeAllItems];
    row = r;
    if (r < 0 || r >= [tv numberOfRows]) return; /* empty space: no menu */
    // Act on the row under the cursor, not a previously selected one.
    [tv selectRowIndexes:[NSIndexSet indexSetWithIndex:(NSUInteger)r] byExtendingSelection:NO];
    unsigned enabled = onOpen ? onOpen((int)r, data) : TABLE_MENU_ALL;
    if (enabled == 0) return;
    for (int i = 0; i < TABLE_MENU_COUNT; i++) {
        if (i == TABLE_MENU_RENAME || i == TABLE_MENU_EXPORT) [menu addItem:[NSMenuItem separatorItem]];
        NSMenuItem *item = [[NSMenuItem alloc] initWithTitle:titles[i] action:@selector(chosen:) keyEquivalent:@""];
        [item setTag:i];
        [item setTarget:self];
        [item setEnabled:(enabled & TABLE_MENU_BIT(i)) != 0];
        [menu addItem:item];
        [item release];
    }
}

- (void)menuNeedsUpdate:(NSMenu *)menu {
    [self fillForRow:[tv clickedRow] menu:menu];
}

- (void)chosen:(NSMenuItem *)item {
    if (!onAction) return;
    table_menu_item which = (table_menu_item)[item tag];
    int r = (int)row;
    // Run after the menu has closed and its tracking loop has unwound, so
    // the action is free to show dialogs or rebuild the table.
    dispatch_async(dispatch_get_main_queue(), ^{ onAction(which, r, data); });
}
@end

static NSTableView *table_view(uiTable *t) {
    NSScrollView *sv = (NSScrollView *) uiControlHandle(uiControl(t));
    id doc = [sv documentView];
    return [doc isKindOfClass:[NSTableView class]] ? doc : nil;
}

void sqlc_table_set_context_menu(uiTable *t, table_menu_open_fn on_open,
                                 table_menu_action_fn on_action, void *data) {
    NSTableView *tv = table_view(t);
    if (!tv) return;
    SqlcTableMenu *m = [SqlcTableMenu new];
    m->tv = tv;
    m->onOpen = on_open;
    m->onAction = on_action;
    m->data = data;
    m->row = -1;
    NSMenu *menu = [[NSMenu alloc] initWithTitle:@"Table"];
    [menu setAutoenablesItems:NO];
    [menu setDelegate:m]; /* weak; the association below keeps m alive */
    [tv setMenu:menu];
    [menu release];
    objc_setAssociatedObject(tv, &menu_key, m, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    [m release];
}

void sqlc_table_menu_describe(uiTable *t, int row, char *buf, size_t size) {
    if (size) buf[0] = '\0';
    NSTableView *tv = table_view(t);
    SqlcTableMenu *m = tv ? objc_getAssociatedObject(tv, &menu_key) : nil;
    NSMenu *menu = [tv menu];
    if (!m || !menu) return;
    [m fillForRow:row menu:menu];
    NSMutableString *s = [NSMutableString string];
    for (NSMenuItem *item in [menu itemArray]) {
        if ([item isSeparatorItem]) continue;
        if ([s length]) [s appendString:@", "];
        [s appendFormat:@"%@%@", [item title], [item isEnabled] ? @"+" : @"-"];
    }
    snprintf(buf, size, "%s", [s UTF8String]);
}

void sqlc_copy_to_clipboard(const char *text) {
    NSString *s = [NSString stringWithUTF8String:text];
    if (!s) return;
    NSPasteboard *pb = [NSPasteboard generalPasteboard];
    [pb clearContents];
    [pb setString:s forType:NSPasteboardTypeString];
}

char *sqlc_clipboard_text(void) {
    NSString *s = [[NSPasteboard generalPasteboard] stringForType:NSPasteboardTypeString];
    const char *u = [s UTF8String];
    return u ? strdup(u) : NULL;
}

char *sqlc_prompt(const char *title, const char *message, const char *initial,
                  const char *ok_label) {
    NSAlert *alert = [[NSAlert alloc] init];
    [alert setMessageText:[NSString stringWithUTF8String:title] ?: @""];
    [alert setInformativeText:[NSString stringWithUTF8String:message] ?: @""];
    [alert addButtonWithTitle:[NSString stringWithUTF8String:ok_label] ?: @"OK"];
    [alert addButtonWithTitle:@"Cancel"];
    NSTextField *field = [[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 280, 24)];
    [field setStringValue:[NSString stringWithUTF8String:initial] ?: @""];
    [alert setAccessoryView:field];
    [[alert window] setInitialFirstResponder:field];
    [alert layout];
    [field selectText:nil];
    char *out = NULL;
    if ([alert runModal] == NSAlertFirstButtonReturn) {
        const char *u = [[field stringValue] UTF8String];
        out = u ? strdup(u) : NULL;
    }
    [field release];
    [alert release];
    return out;
}

int sqlc_confirm_danger(const char *title, const char *message, const char *ok_label,
                        int destructive) {
    NSAlert *alert = [[NSAlert alloc] init];
    [alert setAlertStyle:NSAlertStyleWarning];
    [alert setMessageText:[NSString stringWithUTF8String:title] ?: @""];
    [alert setInformativeText:[NSString stringWithUTF8String:message] ?: @""];
    NSButton *ok = [alert addButtonWithTitle:[NSString stringWithUTF8String:ok_label] ?: @"OK"];
    NSButton *cancel = [alert addButtonWithTitle:@"Cancel"];
    // Cancel is the default: Return (and Escape) choose it, not the action.
    [ok setKeyEquivalent:@""];
    [cancel setKeyEquivalent:@"\r"];
    if (destructive) {
        if (@available(macOS 11.0, *)) [ok setHasDestructiveAction:YES]; // red title
    }
    int confirmed = [alert runModal] == NSAlertFirstButtonReturn;
    [alert release];
    return confirmed;
}
