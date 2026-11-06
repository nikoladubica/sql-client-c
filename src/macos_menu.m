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
