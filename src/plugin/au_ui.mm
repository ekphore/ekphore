// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <ekphore/native_editor.h>

#include "au_ui.h"

#import <AppKit/AppKit.h>
#import <AudioToolbox/AUCocoaUIView.h>

#include <memory>

@interface EkphoreAudioUnitView : NSView
- (instancetype)initWithAudioUnit:(AudioUnit)audioUnit;
@end

@implementation EkphoreAudioUnitView {
    std::unique_ptr<ekphore::EditorSession> _session;
    std::unique_ptr<ekphore::NativeEditor> _editor;
    NSTimer *_timer;
}

- (instancetype)initWithAudioUnit:(AudioUnit)audioUnit {
    ekphore::Plugin *engine{};
    UInt32 size = sizeof(engine);
    if (!audioUnit ||
            AudioUnitGetProperty(audioUnit, ekphore::au::engine_property,
                    kAudioUnitScope_Global, 0, &engine, &size) != noErr ||
            !engine)
        return nil;

    _session = std::make_unique<ekphore::EditorSession>(*engine);
    const auto bounds = _session->size();
    self = [super initWithFrame:NSMakeRect(0, 0, bounds.first, bounds.second)];
    if (!self)
        return nil;
    _editor = std::make_unique<ekphore::NativeEditor>(*_session);
    if (!_editor->attach({ ekphore::NativeWindowApi::cocoa,
                (__bridge void *)self, nullptr }))
        return nil;
    _editor->show();
    _timer = [NSTimer scheduledTimerWithTimeInterval:(1.0 / 60.0)
                                              target:self
                                            selector:@selector(editorIdle:)
                                            userInfo:nil
                                             repeats:YES];
    return self;
}

- (void)editorIdle:(NSTimer *)timer {
    (void)timer;
    if (_editor)
        _editor->idle();
}

- (void)viewDidMoveToWindow {
    [super viewDidMoveToWindow];
    if (!_editor)
        return;
    if (self.window)
        _editor->show();
    else
        _editor->hide();
}

- (void)dealloc {
    [_timer invalidate];
    _timer = nil;
    _editor.reset();
    _session.reset();
}

@end

@interface EkphoreCocoaViewFactory : NSObject <AUCocoaUIBase>
@end

@implementation EkphoreCocoaViewFactory

- (unsigned)interfaceVersion {
    return 0;
}
- (NSString *)description {
    return @"ekphore instrument view";
}

- (NSView *)uiViewForAudioUnit:(AudioUnit)audioUnit
                      withSize:(NSSize)preferredSize {
    (void)preferredSize;
    return [[EkphoreAudioUnitView alloc] initWithAudioUnit:audioUnit];
}

@end
