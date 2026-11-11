// Native macOS menus: the standard Edit menu, Query > Run (Cmd+Return), and
// File > New Tab (Cmd+T) / Close Tab (Cmd+W), plus small control tweaks
// (small buttons, fixed widths, Return in a uiEntry).
// Edit menu: libui-ng doesn't create one, and without it
// Cmd+C / Cmd+V / Cmd+A / Cmd+Z do nothing in text fields.
#import <Cocoa/Cocoa.h>
#import <objc/runtime.h>

#include "macos_menu.h"

static sqlc_menu_callbacks callbacks;

@interface SqlcMenuTarget : NSObject
- (void)runQuery:(id)sender;
- (void)newTab:(id)sender;
- (void)closeTab:(id)sender;
@end

@implementation SqlcMenuTarget
- (void)runQuery:(id)sender {
    (void)sender;
    if (callbacks.run) callbacks.run();
}
- (void)newTab:(id)sender {
    (void)sender;
    if (callbacks.new_tab) callbacks.new_tab();
}
- (void)closeTab:(id)sender {
    (void)sender;
    if (callbacks.close_tab) callbacks.close_tab();
}
@end

static SqlcMenuTarget *menu_target; /* lives for the whole app */

static void add(NSMenu *menu, NSString *title, SEL action, NSString *key,
                NSEventModifierFlags mods) {
    NSMenuItem *item = [[NSMenuItem alloc] initWithTitle:title action:action keyEquivalent:key];
    [item setKeyEquivalentModifierMask:mods];
    [item setTarget:nil]; // first responder
    [menu addItem:item];
    [item release];
}

/* An item calling one of menu_target's actions, inserted at index. */
static void insert_own(NSMenu *menu, NSInteger index, NSString *title, SEL action,
                       NSString *key) {
    NSMenuItem *item = [[NSMenuItem alloc] initWithTitle:title action:action keyEquivalent:key];
    [item setKeyEquivalentModifierMask:NSEventModifierFlagCommand];
    [item setTarget:menu_target];
    [menu insertItem:item atIndex:index];
    [item release];
}

void sqlc_install_menus(const sqlc_menu_callbacks *cb) {
    if (cb) callbacks = *cb;
    // No "Show Tab Bar" / "New Tab" window tabbing from macOS: tabs are sqlc's own.
    [NSWindow setAllowsAutomaticWindowTabbing:NO];
    NSMenu *bar = [NSApp mainMenu];
    if (bar == nil) return;
    if (menu_target == nil) menu_target = [SqlcMenuTarget new];

    NSMenu *edit = [[NSMenu alloc] initWithTitle:@"Edit"];
    const NSEventModifierFlags cmd = NSEventModifierFlagCommand;
    add(edit, @"Undo", @selector(undo:), @"z", cmd);
    add(edit, @"Redo", @selector(redo:), @"z", cmd | NSEventModifierFlagShift);
    [edit addItem:[NSMenuItem separatorItem]];
    add(edit, @"Cut", @selector(cut:), @"x", cmd);
    add(edit, @"Copy", @selector(copy:), @"c", cmd);
    add(edit, @"Paste", @selector(paste:), @"v", cmd);
    add(edit, @"Select All", @selector(selectAll:), @"a", cmd);

    NSMenuItem *holder = [[NSMenuItem alloc] initWithTitle:@"Edit" action:NULL keyEquivalent:@""];
    [holder setSubmenu:edit];
    // Right after the application menu, before libui's own menus.
    [bar insertItem:holder atIndex:([bar numberOfItems] > 0 ? 1 : 0)];
    [holder release];
    [edit release];

    /* Query > Run (Cmd+Return) */
    NSMenu *query = [[NSMenu alloc] initWithTitle:@"Query"];
    NSMenuItem *run = [[NSMenuItem alloc] initWithTitle:@"Run" action:@selector(runQuery:) keyEquivalent:@"\r"];
    [run setKeyEquivalentModifierMask:cmd];
    [run setTarget:menu_target];
    [query addItem:run];
    [run release];

    NSMenuItem *qholder = [[NSMenuItem alloc] initWithTitle:@"Query" action:NULL keyEquivalent:@""];
    [qholder setSubmenu:query];
    [bar insertItem:qholder atIndex:([bar numberOfItems] > 1 ? 2 : [bar numberOfItems])];
    [qholder release];
    [query release];

    /* File > New Tab, Close Tab, then a separator, above libui's items.
     * libui itself binds neither Cmd+T nor Cmd+W. */
    NSMenu *file = [[bar itemWithTitle:@"File"] submenu]; // owned by the bar
    if (file != nil) {
        insert_own(file, 0, @"New Tab", @selector(newTab:), @"t");
        insert_own(file, 1, @"Close Tab", @selector(closeTab:), @"w");
        [file insertItem:[NSMenuItem separatorItem] atIndex:2];
    }
}

int sqlc_confirm(const char *title, const char *message, const char *ok_label) {
    NSAlert *alert = [[NSAlert alloc] init];
    [alert setMessageText:[NSString stringWithUTF8String:title]];
    [alert setInformativeText:[NSString stringWithUTF8String:message]];
    [alert addButtonWithTitle:[NSString stringWithUTF8String:ok_label]];
    [alert addButtonWithTitle:@"Cancel"];
    int ok = [alert runModal] == NSAlertFirstButtonReturn;
    [alert release];
    return ok;
}

void sqlc_button_set_small(uiButton *b) {
    NSButton *btn = (NSButton *) uiControlHandle(uiControl(b));
    [btn setControlSize:NSControlSizeSmall];
    [btn setFont:[NSFont systemFontOfSize:[NSFont systemFontSizeForControlSize:NSControlSizeSmall]]];
    [btn invalidateIntrinsicContentSize];
}

void sqlc_control_set_width(uiControl *c, double width) {
    NSView *v = (NSView *) uiControlHandle(c);
    [v setTranslatesAutoresizingMaskIntoConstraints:NO];
    [[[v widthAnchor] constraintEqualToConstant:width] setActive:YES];
}

void sqlc_notify_done(const char *title, const char *message, int failed) {
    if (![NSApp isActive]) [NSApp requestUserAttention:NSInformationalRequest];
    if (failed) {
        NSBeep();
    } else {
        NSSound *glass = [NSSound soundNamed:@"Glass"]; // shared, not owned
        if (glass) [glass play];
        else NSBeep();
    }
    NSAlert *alert = [[NSAlert alloc] init];
    if (failed) [alert setAlertStyle:NSAlertStyleWarning];
    [alert setMessageText:[NSString stringWithUTF8String:title] ?: @""];
    [alert setInformativeText:[NSString stringWithUTF8String:message] ?: @""];
    [alert addButtonWithTitle:@"OK"];
    [alert runModal];
    [alert release];
}

/* Return in a uiEntry: libui gives a plain entry a delegate (for its
 * on-changed) but no target/action, so NSTextField's action is free. */
@interface SqlcReturnTarget : NSObject {
@public
    void (*fn)(void *);
    void *data;
}
- (void)onReturn:(id)sender;
@end

@implementation SqlcReturnTarget
- (void)onReturn:(id)sender {
    (void)sender;
    /* only Return / Enter (keys 36 / 76), not editing ending on a click elsewhere */
    NSEvent *ev = [NSApp currentEvent];
    if (ev == nil || [ev type] != NSEventTypeKeyDown) return;
    if ([ev keyCode] != 36 && [ev keyCode] != 76) return;
    if (fn) fn(data);
}
@end

static char return_target_key;

void sqlc_entry_on_return(uiEntry *e, void (*f)(void *data), void *data) {
    NSTextField *tf = (NSTextField *) uiControlHandle(uiControl(e));
    SqlcReturnTarget *target = [[SqlcReturnTarget alloc] init];
    target->fn = f;
    target->data = data;
    [tf setTarget:target];   /* not retained by the field ... */
    [tf setAction:@selector(onReturn:)];
    /* ... so the field keeps it (released with the field) */
    objc_setAssociatedObject(tf, &return_target_key, target, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    [target release];
}
