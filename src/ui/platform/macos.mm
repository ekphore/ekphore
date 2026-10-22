// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <ekphore/native_editor.h>

#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>

#include <algorithm>
#include <utility>

@interface EkphoreEditorView : NSView <NSTextInputClient, NSDraggingDestination>
@property(nonatomic, assign) ekphore::EditorSession *session;
@property(nonatomic, strong) NSTrackingArea *trackingArea;
@property(nonatomic, strong) NSMutableAttributedString *markedText;
@property(nonatomic, assign) BOOL arrayDialogOpen;
@end

@implementation EkphoreEditorView

- (BOOL)isFlipped {
    return YES;
}
- (void)serviceArrayDialog {
    if (!self.session || !self.window || self.arrayDialogOpen)
        return;
    if (self.session->folder_selection_requested() ||
            self.session->key_selection_requested()) {
        const bool key = self.session->key_selection_requested();
        auto *session = self.session;
        self.arrayDialogOpen = YES;
        NSOpenPanel *panel = [NSOpenPanel openPanel];
        panel.canChooseDirectories = !key;
        panel.canChooseFiles = key;
        panel.allowsMultipleSelection = NO;
        panel.prompt = key ? @"Choose key file" : @"Choose instrument folder";
        __weak EkphoreEditorView *weakView = self;
        [panel beginSheetModalForWindow:self.window
                      completionHandler:^(NSModalResponse result) {
                        EkphoreEditorView *view = weakView;
                        if (!view)
                            return;
                        view.arrayDialogOpen = NO;
                        if (view.session != session)
                            return;
                        std::optional<std::filesystem::path> path;
                        if (result == NSModalResponseOK && panel.URL)
                            path = panel.URL.fileSystemRepresentation;
                        if (key)
                            session->select_encryption_key(path);
                        else
                            session->select_instrument_folder(path);
                        [view setNeedsDisplay:YES];
                      }];
        return;
    }
    const auto requests = self.session->array_file_requests();
    if (requests.empty())
        return;
    const auto request = requests.front();
    auto *session = self.session;
    self.arrayDialogOpen = YES;
    NSSavePanel *panel =
            request.save ? [NSSavePanel savePanel] : [NSOpenPanel openPanel];
    panel.allowedFileTypes = @[@"nka"];
    if (request.save) {
        const auto filename = request.array_name.substr(1) + ".nka";
        panel.nameFieldStringValue =
                [NSString stringWithUTF8String:filename.c_str()];
    } else {
        NSOpenPanel *open = (NSOpenPanel *)panel;
        open.canChooseDirectories = NO;
        open.allowsMultipleSelection = NO;
    }
    __weak EkphoreEditorView *weakView = self;
    [panel beginSheetModalForWindow:self.window
                  completionHandler:^(NSModalResponse result) {
                    EkphoreEditorView *view = weakView;
                    if (!view)
                        return;
                    view.arrayDialogOpen = NO;
                    if (view.session != session)
                        return;
                    std::optional<std::filesystem::path> path;
                    if (result == NSModalResponseOK && panel.URL)
                        path = panel.URL.fileSystemRepresentation;
                    (void)session->select_array_file(request.command, path);
                    [view setNeedsDisplay:YES];
                  }];
}
- (BOOL)acceptsFirstResponder {
    return YES;
}
- (BOOL)canBecomeKeyView {
    return YES;
}
- (BOOL)isOpaque {
    return YES;
}

- (void)updateTrackingAreas {
    if (self.trackingArea)
        [self removeTrackingArea:self.trackingArea];
    const auto options = NSTrackingMouseEnteredAndExited |
                         NSTrackingMouseMoved | NSTrackingActiveAlways |
                         NSTrackingInVisibleRect;
    self.trackingArea = [[NSTrackingArea alloc] initWithRect:self.bounds
                                                     options:options
                                                       owner:self
                                                    userInfo:nil];
    [self addTrackingArea:self.trackingArea];
    [super updateTrackingAreas];
}

- (NSPoint)editorPoint:(NSEvent *)event {
    return [self convertPoint:event.locationInWindow fromView:nil];
}

- (void)drawRect:(NSRect)dirtyRect {
    (void)dirtyRect;
    if (!self.session)
        return;
    const CGFloat backingScale =
            self.window ? self.window.backingScaleFactor
                        : NSScreen.mainScreen.backingScaleFactor;
    auto frame = self.session->render(static_cast<float>(backingScale));
    if (frame.rgba.empty() || frame.width == 0 || frame.height == 0)
        return;

    auto *data = CFDataCreate(nullptr, frame.rgba.data(),
            static_cast<CFIndex>(frame.rgba.size()));
    if (!data)
        return;
    auto *provider = CGDataProviderCreateWithCFData(data);
    CFRelease(data);
    if (!provider)
        return;
    const auto color_space = CGColorSpaceCreateDeviceRGB();
    const auto bitmap_info = static_cast<CGBitmapInfo>(
            static_cast<std::uint32_t>(kCGImageAlphaPremultipliedLast) |
            static_cast<std::uint32_t>(kCGBitmapByteOrder32Big));
    auto *image = CGImageCreate(frame.width, frame.height, 8, 32, frame.stride,
            color_space, bitmap_info, provider, nullptr, false,
            kCGRenderingIntentDefault);
    CGColorSpaceRelease(color_space);
    CGDataProviderRelease(provider);
    if (!image)
        return;
    auto *context = NSGraphicsContext.currentContext.CGContext;
    // still need filtering for fractional zoom and monitor changes
    CGContextSetInterpolationQuality(context, kCGInterpolationHigh);
    // our rows go top to bottom, flip or Core Graphics mirrors everything
    CGContextSaveGState(context);
    CGContextTranslateCTM(context, 0.0, self.bounds.size.height);
    CGContextScaleCTM(context, 1.0, -1.0);
    CGContextDrawImage(context, NSRectToCGRect(self.bounds), image);
    CGContextRestoreGState(context);
    CGImageRelease(image);
}

- (void)mouseDown:(NSEvent *)event {
    if (!self.session)
        return;
    [self.window makeFirstResponder:self];
    const auto point = [self editorPoint:event];
    if (self.session->pointer_down(point.x, point.y,
                static_cast<std::uint32_t>(self.bounds.size.width),
                static_cast<std::uint32_t>(self.bounds.size.height)))
        [self setNeedsDisplay:YES];
}

- (void)scrollWheel:(NSEvent *)event {
    if (!self.session)
        return;
    const auto point = [self editorPoint:event];
    const double pixels =
            -event.scrollingDeltaY * (event.hasPreciseScrollingDeltas ? 1 : 72);
    if (self.session->pointer_scroll(point.x, point.y, pixels,
                static_cast<std::uint32_t>(self.bounds.size.width),
                static_cast<std::uint32_t>(self.bounds.size.height)))
        [self setNeedsDisplay:YES];
}

- (void)mouseDragged:(NSEvent *)event {
    if (!self.session)
        return;
    const auto point = [self editorPoint:event];
    if (self.session->pointer_drag(point.x, point.y,
                static_cast<std::uint32_t>(self.bounds.size.width),
                static_cast<std::uint32_t>(self.bounds.size.height)))
        [self setNeedsDisplay:YES];
}

- (void)mouseMoved:(NSEvent *)event {
    if (!self.session)
        return;
    const auto point = [self editorPoint:event];
    if (self.session->pointer_move(point.x, point.y,
                static_cast<std::uint32_t>(self.bounds.size.width),
                static_cast<std::uint32_t>(self.bounds.size.height)))
        [self setNeedsDisplay:YES];
}

- (void)mouseUp:(NSEvent *)event {
    (void)event;
    if (!self.session)
        return;
    self.session->pointer_up();
    [self setNeedsDisplay:YES];
}

- (void)keyDown:(NSEvent *)event {
    if (!self.session)
        return;
    const auto flags = event.modifierFlags;
    const bool shift = (flags & NSEventModifierFlagShift) != 0;
    const bool control = (flags & NSEventModifierFlagCommand) != 0;
    if (control && event.charactersIgnoringModifiers.length != 0) {
        const unichar character =
                [event.charactersIgnoringModifiers characterAtIndex:0];
        if (character == 'a' || character == 'A') {
            if (self.session->key_down(ekphore::UiKey::select_all, false, true))
                [self setNeedsDisplay:YES];
            return;
        }
        if (character == 'v' || character == 'V') {
            NSString *value = [[NSPasteboard generalPasteboard]
                    stringForType:NSPasteboardTypeString];
            const char *utf8 = value.UTF8String;
            if (utf8 && self.session->text_input(utf8))
                [self setNeedsDisplay:YES];
            return;
        }
    }
    ekphore::UiKey key = ekphore::UiKey::escape;
    bool translated = true;
    switch (event.keyCode) {
        case 36:
            key = ekphore::UiKey::enter;
            break;
        case 48:
            key = ekphore::UiKey::tab;
            break;
        case 51:
            key = ekphore::UiKey::backspace;
            break;
        case 53:
            key = ekphore::UiKey::escape;
            break;
        case 117:
            key = ekphore::UiKey::delete_forward;
            break;
        case 123:
            key = ekphore::UiKey::left;
            break;
        case 124:
            key = ekphore::UiKey::right;
            break;
        case 125:
            key = ekphore::UiKey::down;
            break;
        case 126:
            key = ekphore::UiKey::up;
            break;
        default:
            translated = false;
            break;
    }
    if (translated && self.session->key_down(key, shift, control)) {
        [self setNeedsDisplay:YES];
        return;
    }
    [self interpretKeyEvents:@[event]];
}

- (void)insertText:(id)text replacementRange:(NSRange)range {
    (void)range;
    NSString *value = [text isKindOfClass:[NSAttributedString class]]
                              ? [text string]
                              : text;
    if (!self.session || !value)
        return;
    const char *utf8 = value.UTF8String;
    if (utf8 && self.session->text_input(utf8))
        [self setNeedsDisplay:YES];
    [self unmarkText];
}

- (void)setMarkedText:(id)text
           selectedRange:(NSRange)selectedRange
        replacementRange:(NSRange)replacementRange {
    (void)selectedRange;
    (void)replacementRange;
    NSAttributedString *value =
            [text isKindOfClass:[NSAttributedString class]]
                    ? text
                    : [[NSAttributedString alloc] initWithString:text];
    self.markedText = [value mutableCopy];
}
- (void)unmarkText {
    self.markedText = nil;
}
- (BOOL)hasMarkedText {
    return self.markedText.length != 0;
}
- (NSRange)markedRange {
    return self.hasMarkedText ? NSMakeRange(0, self.markedText.length)
                              : NSMakeRange(NSNotFound, 0);
}
- (NSRange)selectedRange {
    return NSMakeRange(NSNotFound, 0);
}
- (NSArray<NSAttributedStringKey> *)validAttributesForMarkedText {
    return @[];
}
- (NSAttributedString *)attributedSubstringForProposedRange:(NSRange)range
                                                actualRange:
                                                        (NSRangePointer)actual {
    (void)range;
    if (actual)
        *actual = NSMakeRange(NSNotFound, 0);
    return nil;
}
- (NSUInteger)characterIndexForPoint:(NSPoint)point {
    (void)point;
    return NSNotFound;
}
- (NSRect)firstRectForCharacterRange:(NSRange)range
                         actualRange:(NSRangePointer)actual {
    (void)range;
    if (actual)
        *actual = NSMakeRange(NSNotFound, 0);
    return [self.window convertRectToScreen:self.frame];
}
- (void)doCommandBySelector:(SEL)selector {
    (void)selector;
}

- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)sender {
    NSPasteboard *pasteboard = sender.draggingPasteboard;
    NSArray<NSURL *> *urls = [pasteboard
            readObjectsForClasses:@[[NSURL class]]
                          options:@{
                              NSPasteboardURLReadingFileURLsOnlyKey: @YES
                          }];
    for (NSURL *url in urls)
        if ([[url.path.pathExtension lowercaseString] isEqualToString:@"nki"] ||
                [[url.path.pathExtension lowercaseString]
                        isEqualToString:@"nksn"])
            return NSDragOperationCopy;
    return NSDragOperationNone;
}

- (BOOL)performDragOperation:(id<NSDraggingInfo>)sender {
    if (!self.session)
        return NO;
    NSArray<NSURL *> *urls = [sender.draggingPasteboard
            readObjectsForClasses:@[[NSURL class]]
                          options:@{
                              NSPasteboardURLReadingFileURLsOnlyKey: @YES
                          }];
    std::vector<std::filesystem::path> files;
    files.reserve(urls.count);
    for (NSURL *url in urls)
        if (url.isFileURL && url.path.UTF8String)
            files.emplace_back(url.path.UTF8String);
    if (files.empty())
        return NO;
    const NSPoint point = [self convertPoint:sender.draggingLocation
                                    fromView:nil];
    const bool accepted = self.session->drop_files(point.x, point.y,
            static_cast<std::uint32_t>(self.bounds.size.width),
            static_cast<std::uint32_t>(self.bounds.size.height), files);
    if (accepted)
        [self setNeedsDisplay:YES];
    return accepted ? YES : NO;
}

@end

namespace ekphore {

class NativeEditor::Implementation {
  public:
    explicit Implementation(EditorSession &session) : session(session) {}
    ~Implementation() { detach(); }

    bool attach(const NativeWindowParent &parent) {
        if (parent.api != NativeWindowApi::cocoa || !parent.handle)
            return false;
        detach();
        auto *parent_view = (__bridge NSView *)parent.handle;
        const auto [width, height] = session.size();
        view = [[EkphoreEditorView alloc]
                initWithFrame:NSMakeRect(0, 0, width, height)];
        view.session = &session;
        view.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
        [view registerForDraggedTypes:@[NSPasteboardTypeFileURL]];
        [parent_view addSubview:view];
        session.set_visible(true);
        [view setNeedsDisplay:YES];
        __weak EkphoreEditorView *weak_view = view;
        timer = [NSTimer
                scheduledTimerWithTimeInterval:(1.0 / 60.0)
                                       repeats:YES
                                         block:^(NSTimer *) {
                                           EkphoreEditorView *strong_view =
                                                   weak_view;
                                           if (!strong_view ||
                                                   strong_view.hidden ||
                                                   !strong_view.session)
                                               return;
                                           [strong_view serviceArrayDialog];
                                           strong_view.session->invalidate();
                                           [strong_view setNeedsDisplay:YES];
                                         }];
        return true;
    }

    void detach() {
        if (!view)
            return;
        [timer invalidate];
        timer = nil;
        view.session = nullptr;
        [view removeFromSuperview];
        view = nil;
        session.set_visible(false);
    }
    void show() {
        if (view) {
            view.hidden = NO;
            session.set_visible(true);
        }
    }
    void hide() {
        if (view) {
            view.hidden = YES;
            session.set_visible(false);
        }
    }
    void idle() {
        if (!view || !session.visible())
            return;
        [view serviceArrayDialog];
        session.invalidate();
        [view setNeedsDisplay:YES];
    }
    bool attached() const { return view != nil; }
    void *native_handle() const { return (__bridge void *)view; }
    std::pair<std::uint32_t, std::uint32_t> size() const {
        return session.size();
    }
    bool set_scale(const float scale) {
        session.set_scale(scale);
        if (!view)
            return true;
        const auto [width, height] = session.size();
        [view setFrameSize:NSMakeSize(width, height)];
        [view setNeedsDisplay:YES];
        return true;
    }

  private:
    EditorSession &session;
    __strong EkphoreEditorView *view{};
    __strong NSTimer *timer{};
};

NativeEditor::NativeEditor(EditorSession &session)
    : implementation(std::make_unique<Implementation>(session)) {}
NativeEditor::~NativeEditor() = default;
NativeEditor::NativeEditor(NativeEditor &&) noexcept = default;
NativeEditor &NativeEditor::operator=(NativeEditor &&) noexcept = default;
bool NativeEditor::supports(const NativeWindowApi api) {
    return api == NativeWindowApi::cocoa;
}
bool NativeEditor::attach(const NativeWindowParent &parent) {
    return implementation->attach(parent);
}
void NativeEditor::detach() { implementation->detach(); }
void NativeEditor::show() { implementation->show(); }
void NativeEditor::hide() { implementation->hide(); }
void NativeEditor::idle() { implementation->idle(); }
bool NativeEditor::attached() const { return implementation->attached(); }
void *NativeEditor::native_handle() const {
    return implementation->native_handle();
}
std::pair<std::uint32_t, std::uint32_t> NativeEditor::size() const {
    return implementation->size();
}
bool NativeEditor::set_scale(const float scale) {
    return implementation->set_scale(scale);
}

} // namespace ekphore
