// Everything Swift is allowed to see from the native side.
// Set as SWIFT_OBJC_BRIDGING_HEADER in project.yml.
//
//   Swift (Frontend/)  ->  Obj-C++ (Bridge/*.mm)  ->  C API (core_api.h)  ->  C++ core
//
// Only C and Objective-C may appear in the headers below — no C++ types —
// because Swift's importer parses them as Objective-C.
#import "ember/core_api.h"   // em_pad_state and em_core_read_audio are used directly from Swift

#import "EMEmulatorCore.h"
#import "EMJITSupport.h"
#import "EMPipelineCache.h"
