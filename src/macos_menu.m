// Native macOS menus: the standard Edit menu, plus Query > Run (Cmd+Return).
// Edit menu: libui-ng doesn't create one, and without it
// Cmd+C / Cmd+V / Cmd+A / Cmd+Z do nothing in text fields.
#import <Cocoa/Cocoa.h>

#include "macos_menu.h"

static void (*run_callback)(void);

@interface SqlcMenuTarget : NSObject
- (void)runQuery:(id)sender;
@end

@implementation SqlcMenuTarget
- (void)runQuery:(id)sender {
    (void)sender;
    if (run_callback) run_callback();
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

void sqlc_install_menus(void (*on_run)(void)) {
    NSMenu *bar = [NSApp mainMenu];
    if (bar == nil) return;

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
    run_callback = on_run;
    menu_target = [SqlcMenuTarget new];
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
