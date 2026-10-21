"""Exercise stream target selection and cached-app host rebinding without networking."""
from pathlib import Path
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[2]
controller = (root / 'Limelight/macOS/ViewControllers/StreamViewController.m').read_text()
apps = (root / 'Limelight/macOS/ViewControllers/AppsViewController.m').read_text()
store = (root / 'Limelight/Network/ConnectionEndpointStore.m').read_text()


def method(source, signature):
    start = source.index(signature)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        if source[end] == '{': depth += 1
        elif source[end] == '}': depth -= 1
        end += 1
    return source[start:end]


selection = controller[controller.index('    streamConfig.hostUUID = self.app.host.uuid;'):]
selection = selection[:selection.index('    Log(LOG_I, @"[diag] Stream target selection:')]
segue = apps[apps.index('- (void)prepareForSegue:'):]
segue = segue[segue.index('    StreamViewController *streamVC'):segue.index('    // Pending overrides')]
resolver = method(store, '+ (NSString *)streamEndpointForHost:')
fixture = r'''
#import <Foundation/Foundation.h>
#include <assert.h>
#define LOG_E 1
#define Log(...) ((void)0)
#define MLString(key,comment) key
@interface TemporaryHost : NSObject
@property NSString *uuid,*activeAddress;
@property NSDictionary<NSString *,NSNumber *> *addressStates;
@property NSArray<NSString *> *endpoints;
@end
@implementation TemporaryHost
@end
@interface App : NSObject
@property TemporaryHost *host;
@end
@implementation App
@end
@interface StreamConfiguration : NSObject
@property NSString *host,*hostUUID;
@end
@implementation StreamConfiguration
@end
@interface SettingsClass : NSObject
+ (NSDictionary *)getSettingsFor:(NSString *)uuid;
@end
static NSDictionary *settings;
@implementation SettingsClass
+ (NSDictionary *)getSettingsFor:(NSString *)uuid { return settings; }
@end
@interface ConnectionEndpointStore : NSObject
+ (NSArray<NSString *> *)allEndpointsForHost:(TemporaryHost *)host;
+ (NSString *)normalizedAddress:(NSString *)address;
+ (NSString *)streamEndpointForHost:(TemporaryHost *)host connectionMethod:(NSString *)method;
@end
@implementation ConnectionEndpointStore
+ (NSArray<NSString *> *)allEndpointsForHost:(TemporaryHost *)host { return host.endpoints ?: @[]; }
+ (NSString *)normalizedAddress:(NSString *)address { return address ?: @""; }
''' + resolver + r'''
@end
@interface StreamViewController : NSObject
@property App *app;
@property id delegate;
@property int launchFailures;
- (void)selectInto:(StreamConfiguration *)streamConfig;
- (void)launchFailed:(NSString *)message;
@end
@implementation StreamViewController
- (void)launchFailed:(NSString *)message { self.launchFailures++; }
- (void)selectInto:(StreamConfiguration *)streamConfig {
''' + selection + r'''
}
@end
@interface NSStoryboardSegue : NSObject
@property StreamViewController *destinationController;
@end
@implementation NSStoryboardSegue
@end
@interface Apps : NSObject
@property App *runningApp;
@property TemporaryHost *host;
- (void)prepareForSegue:(NSStoryboardSegue *)segue sender:(id)sender;
@end
@implementation Apps
- (void)prepareForSegue:(NSStoryboardSegue *)segue sender:(id)sender {
''' + segue + r'''
}
@end
static TemporaryHost *host(NSString *uuid, NSString *active, NSArray *endpoints, NSDictionary *states) {
 TemporaryHost *h=[TemporaryHost new];h.uuid=uuid;h.activeAddress=active;h.endpoints=endpoints;h.addressStates=states;return h;
}
static StreamViewController *stream(TemporaryHost *h) {
 StreamViewController *s=[StreamViewController new];s.app=[App new];s.app.host=h;return s;
}
static NSString *selectTarget(StreamViewController *s, NSString *method) {
 settings=method ? @{@"connectionMethod":method} : @{};
 StreamConfiguration *config=[StreamConfiguration new];[s selectInto:config];return config.host;
}
int main(void) { @autoreleasepool {
 // Reproduce the log: cached app host has no active address despite a known LAN endpoint.
 StreamViewController *s=stream(host(@"one",nil,@[@"192.0.2.10",@"host.local"],@{@"192.0.2.10":@1}));
 assert([selectTarget(s,@"Auto") isEqualToString:@"192.0.2.10"]);
 assert([selectTarget(s,@"Auto") isEqualToString:@"192.0.2.10"]); // Retry in the same window.
 // Manual targets retain priority even if discovery reports them offline.
 assert([selectTarget(s,@"vpn.example:57989") isEqualToString:@"vpn.example:57989"]);
 // Keep the currently selected enabled route, including an unknown discovery state.
 s.app.host=host(@"one",@"2001:db8::10",@[@"192.0.2.10",@"2001:db8::10"],@{@"192.0.2.10":@1});
 assert([selectTarget(s,nil) isEqualToString:@"2001:db8::10"]);
 // Disabled routes are excluded by allEndpointsForHost; offline routes are not selected.
 s.app.host=host(@"one",@"disabled",@[@"offline",@"vpn.example:57989"],@{@"offline":@0,@"vpn.example:57989":@1});
 assert([selectTarget(s,@"Auto") isEqualToString:@"vpn.example:57989"]);
 s.app.host=host(@"one",nil,@[@"offline",@"[2001:db8::20]:57989"],@{@"offline":@0});
 assert([selectTarget(s,@"Auto") isEqualToString:@"[2001:db8::20]:57989"]);
 s.app.host=host(@"one",nil,@[],@{});
 assert(selectTarget(s,@"Auto")==nil && s.launchFailures==1);
 s.app.host=host(@"one",nil,@[@"offline"],@{@"offline":@0});
 assert(selectTarget(s,@"Auto")==nil && s.launchFailures==2);
 puts("PASS: first launch, same-window retry, manual priority, active/disabled/offline routes, custom ports and empty hosts");
 // A cached app must share current host updates, but never be rebound to another PC.
 Apps *apps=[Apps new];apps.runningApp=[App new];
 apps.runningApp.host=host(@"one",nil,@[],@{});
 apps.host=host(@"one",@"192.0.2.10",@[],@{});
 NSStoryboardSegue *segue=[NSStoryboardSegue new];segue.destinationController=[StreamViewController new];
 [apps prepareForSegue:segue sender:nil]; assert(segue.destinationController.app.host==apps.host);
 apps.host.activeAddress=@"192.0.2.11";
 assert([segue.destinationController.app.host.activeAddress isEqualToString:@"192.0.2.11"]);
 TemporaryHost *original=apps.runningApp.host;apps.host=host(@"another",@"192.0.2.12",@[],@{});
 [apps prepareForSegue:segue sender:nil];assert(apps.runningApp.host==original);
 puts("PASS: cached app rebinding follows current host updates and rejects a different UUID");
} }
'''
with tempfile.TemporaryDirectory(prefix='moonlight-startup-endpoint-') as tmp:
    path = Path(tmp)
    (path/'test.m').write_text(fixture)
    subprocess.run(['clang','-fobjc-arc','-framework','Foundation',str(path/'test.m'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)

for script in ('retry_state.py', 'failed_cleanup.py'):
    subprocess.run([sys.executable, str(root/'tests/startup-endpoint'/script)], check=True)
