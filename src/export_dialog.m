// Export options dialog + save panel (libui-ng has no way to build a modal
// dialog with custom controls, nor a save panel with a suggested name).
#import <Cocoa/Cocoa.h>

#include <string.h>

#include "export_dialog.h"

/* remembered for the session */
static int have_last;
static export_format last_fmt;
static export_opts last_opts;

@interface SqlcFlippedView : NSView
@end
@implementation SqlcFlippedView
- (BOOL)isFlipped { return YES; }
@end

@interface SqlcExportControls : NSObject {
@public
    NSPopUpButton *format;
    NSButton *structure, *drop, *data;  /* SQL */
    NSButton *nullN, *bom;              /* CSV */
    NSButton *ok;
    BOOL isView;
}
- (void)update:(id)sender;
@end

@implementation SqlcExportControls
- (void)update:(id)sender {
    (void)sender;
    NSInteger f = [format indexOfSelectedItem];
    BOOL sql = f == EXPORT_SQL, csv = f == EXPORT_CSV;
    BOOL wantStructure = [structure state] == NSControlStateValueOn;
    [structure setEnabled:sql];
    [drop setEnabled:sql && wantStructure];
    [data setEnabled:sql && !isView];
    [nullN setEnabled:csv];
    [bom setEnabled:csv];
    // SQL needs at least one of structure / data.
    BOOL wantData = !isView && [data state] == NSControlStateValueOn;
    [ok setEnabled:!sql || wantStructure || wantData];
}
@end

static NSButton *checkbox(NSView *parent, NSString *title, BOOL on, CGFloat y, CGFloat indent,
                          SqlcExportControls *target) {
    NSButton *b = [[NSButton alloc] initWithFrame:NSMakeRect(indent, y, 340 - indent, 20)];
    [b setButtonType:NSButtonTypeSwitch];
    [b setTitle:title];
    [b setState:on ? NSControlStateValueOn : NSControlStateValueOff];
    [b setTarget:target];
    [b setAction:@selector(update:)];
    [parent addSubview:b];
    [b release];
    return b;
}

static NSTextField *heading(NSView *parent, NSString *text, CGFloat y) {
    NSTextField *t = [NSTextField labelWithString:text];
    [t setFont:[NSFont boldSystemFontOfSize:[NSFont smallSystemFontSize]]];
    [t setFrame:NSMakeRect(0, y, 340, 16)];
    [parent addSubview:t];
    return t;
}

int sqlc_export_dialog(const char *table, int is_view,
                       export_format *fmt, export_opts *opts, char **path) {
    if (!have_last) {
        last_fmt = EXPORT_SQL;
        last_opts = export_default_opts();
        have_last = 1;
    }
    NSString *name = [NSString stringWithUTF8String:table];
    if (!name) name = @"table";

    NSAlert *alert = [[NSAlert alloc] init];
    [alert setMessageText:[NSString stringWithFormat:@"Export %@ “%@”", is_view ? @"view" : @"table", name]];
    [alert setInformativeText:@"Choose a format, then where to save the file."];
    NSButton *ok = [alert addButtonWithTitle:@"Export…"];
    [alert addButtonWithTitle:@"Cancel"];

    SqlcExportControls *c = [SqlcExportControls new];
    c->isView = is_view;
    c->ok = ok;

    NSView *v = [[SqlcFlippedView alloc] initWithFrame:NSMakeRect(0, 0, 340, 214)];
    NSTextField *flabel = [NSTextField labelWithString:@"Format:"];
    [flabel setFrame:NSMakeRect(0, 3, 60, 18)];
    [v addSubview:flabel];
    c->format = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(62, 0, 200, 26) pullsDown:NO];
    [c->format addItemsWithTitles:@[ @"SQL (CREATE + INSERT)", @"CSV", @"XML" ]];
    [c->format selectItemAtIndex:last_fmt];
    [c->format setTarget:c];
    [c->format setAction:@selector(update:)];
    [v addSubview:c->format];
    [c->format release];

    heading(v, @"SQL", 40);
    c->structure = checkbox(v, is_view ? @"Structure (CREATE VIEW)" : @"Structure (CREATE TABLE)",
                            last_opts.sql_structure, 60, 0, c);
    c->drop = checkbox(v, is_view ? @"Add DROP VIEW IF EXISTS" : @"Add DROP TABLE IF EXISTS",
                       last_opts.sql_drop, 82, 18, c);
    c->data = checkbox(v, is_view ? @"Data (not exported for views)" : @"Data (INSERT statements)",
                       last_opts.sql_data && !is_view, 104, 0, c);

    heading(v, @"CSV", 138);
    c->nullN = checkbox(v, @"Write NULL as \\N (otherwise an empty field)", last_opts.csv_null_as_n, 158, 0, c);
    c->bom = checkbox(v, @"Start with a UTF-8 BOM (helps Excel)", last_opts.csv_bom, 180, 0, c);

    [alert setAccessoryView:v];
    [v release];
    [c update:nil];

    int confirmed = [alert runModal] == NSAlertFirstButtonReturn;
    export_format f = (export_format)[c->format indexOfSelectedItem];
    export_opts o = {
        .sql_structure = [c->structure state] == NSControlStateValueOn,
        .sql_drop = [c->drop state] == NSControlStateValueOn,
        .sql_data = !is_view && [c->data state] == NSControlStateValueOn,
        .csv_null_as_n = [c->nullN state] == NSControlStateValueOn,
        .csv_bom = [c->bom state] == NSControlStateValueOn,
    };
    [c release];
    [alert release];
    if (!confirmed) return 0;

    last_fmt = f;
    if (!is_view) last_opts = o; /* a view's forced "no data" shouldn't stick */
    else { int d = last_opts.sql_data; last_opts = o; last_opts.sql_data = d; }

    NSSavePanel *panel = [NSSavePanel savePanel];
    NSString *ext = [NSString stringWithUTF8String:export_extension(f)];
    NSString *safe = [[name stringByReplacingOccurrencesOfString:@"/" withString:@"-"]
                          stringByReplacingOccurrencesOfString:@":" withString:@"-"];
    [panel setNameFieldStringValue:[safe stringByAppendingPathExtension:ext]];
    [panel setCanCreateDirectories:YES];
    [panel setExtensionHidden:NO];
    if ([panel runModal] != NSModalResponseOK) return 0;
    const char *p = [[[panel URL] path] fileSystemRepresentation];
    if (!p || !(*path = strdup(p))) return 0;
    *fmt = f;
    *opts = o;
    return 1;
}
