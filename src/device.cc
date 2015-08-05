#include "device.h"

#include "application.h"
#include "events.h"
#include "icon.h"
#include "operation.h"
#include "process.h"
#include "session.h"

#include <nan.h>
#include <node.h>

#define DEVICE_DATA_CONSTRUCTOR "device:ctor"

using v8::AccessorSignature;
using v8::DEFAULT;
using v8::External;
using v8::Function;
using v8::Handle;
using v8::Isolate;
using v8::Local;
using v8::Object;
using v8::ReadOnly;
using v8::String;
using v8::Value;
using Nan::HandleScope;

namespace frida {

Device::Device(FridaDevice* handle, Runtime* runtime)
    : GLibObject(handle, runtime) {
  g_object_ref(handle_);
}

Device::~Device() {
  events_.Reset();
  frida_unref(handle_);
}

void Device::Init(Handle<Object> exports, Runtime* runtime) {
  auto isolate = Isolate::GetCurrent();

  auto name = Nan::New("Device").ToLocalChecked();
  auto tpl = CreateTemplate(name, New, runtime);

  auto instance_tpl = tpl->InstanceTemplate();
  auto data = Handle<Value>();
  auto signature = AccessorSignature::New(isolate, tpl);
  Nan::SetAccessor(instance_tpl, Nan::New("id").ToLocalChecked(), GetId, 0,
      data, DEFAULT, ReadOnly, signature);
  Nan::SetAccessor(instance_tpl, Nan::New("name").ToLocalChecked(), GetName, 0,
      data, DEFAULT, ReadOnly, signature);
  Nan::SetAccessor(instance_tpl, Nan::New("icon").ToLocalChecked(), GetIcon, 0,
      data, DEFAULT, ReadOnly, signature);
  Nan::SetAccessor(instance_tpl, Nan::New("type").ToLocalChecked(), GetType, 0,
      data, DEFAULT, ReadOnly, signature);

  Nan::SetPrototypeMethod(tpl, "getFrontmostApplication",
      GetFrontmostApplication);
  Nan::SetPrototypeMethod(tpl, "enumerateApplications", EnumerateApplications);
  Nan::SetPrototypeMethod(tpl, "enumerateProcesses", EnumerateProcesses);
  Nan::SetPrototypeMethod(tpl, "spawn", Spawn);
  Nan::SetPrototypeMethod(tpl, "resume", Resume);
  Nan::SetPrototypeMethod(tpl, "kill", Kill);
  Nan::SetPrototypeMethod(tpl, "attach", Attach);

  auto ctor = Nan::GetFunction(tpl).ToLocalChecked();
  Nan::Set(exports, name, ctor);
  runtime->SetDataPointer(DEVICE_DATA_CONSTRUCTOR,
      new v8::Persistent<v8::Function>(isolate, ctor));
}

Local<Object> Device::New(gpointer handle, Runtime* runtime) {
  auto ctor = Nan::New<Function>(
      *static_cast<v8::Persistent<Function>*>(
      runtime->GetDataPointer(DEVICE_DATA_CONSTRUCTOR)));
  const int argc = 1;
  Local<Value> argv[argc] = { Nan::New<v8::External>(handle) };
  return ctor->NewInstance(argc, argv);
}

NAN_METHOD(Device::New) {
  HandleScope scope;

  if (info.IsConstructCall()) {
    if (info.Length() != 1 || !info[0]->IsExternal()) {
      Nan::ThrowTypeError("Bad argument, expected raw handle");
      return;
    }
    auto runtime = GetRuntimeFromConstructorArgs(info);

    auto handle = static_cast<FridaDevice*>(
        Local<External>::Cast(info[0])->Value());
    auto wrapper = new Device(handle, runtime);
    auto obj = info.This();
    wrapper->Wrap(obj);
    Nan::Set(obj, Nan::New("events").ToLocalChecked(),
        Events::New(handle, runtime));

    info.GetReturnValue().Set(obj);
  } else {
    info.GetReturnValue().Set(info.Callee()->NewInstance(0, NULL));
  }
}

NAN_PROPERTY_GETTER(Device::GetId) {
  HandleScope scope;

  auto handle = ObjectWrap::Unwrap<Device>(
      info.Holder())->GetHandle<FridaDevice>();

  info.GetReturnValue().Set(Nan::New<v8::Uint32>(
    frida_device_get_id(handle)));
}

NAN_PROPERTY_GETTER(Device::GetName) {
  HandleScope scope;

  auto handle = ObjectWrap::Unwrap<Device>(
      info.Holder())->GetHandle<FridaDevice>();

  info.GetReturnValue().Set(
      Nan::New(frida_device_get_name(handle)).ToLocalChecked());
}

NAN_PROPERTY_GETTER(Device::GetIcon) {
  HandleScope scope;

  auto wrapper = ObjectWrap::Unwrap<Device>(info.Holder());
  auto handle = wrapper->GetHandle<FridaDevice>();

  info.GetReturnValue().Set(Icon::New(frida_device_get_icon(handle),
      wrapper->runtime_));
}

NAN_PROPERTY_GETTER(Device::GetType) {
  HandleScope scope;

  auto handle = ObjectWrap::Unwrap<Device>(
      info.Holder())->GetHandle<FridaDevice>();

  const gchar* type;
  switch (frida_device_get_dtype(handle)) {
    case FRIDA_DEVICE_TYPE_LOCAL:
      type = "local";
      break;
    case FRIDA_DEVICE_TYPE_TETHER:
      type = "tether";
      break;
    case FRIDA_DEVICE_TYPE_REMOTE:
      type = "remote";
      break;
    default:
      g_assert_not_reached();
  }

  info.GetReturnValue().Set(Nan::New(type).ToLocalChecked());
}

class GetFrontmostApplicationOperation : public Operation<FridaDevice> {
 public:
  void Begin() {
    frida_device_get_frontmost_application(handle_, OnReady, this);
  }

  void End(GAsyncResult* result, GError** error) {
    application_ = frida_device_get_frontmost_application_finish(handle_,
        result, error);
  }

  Local<Value> Result(Isolate* isolate) {
    if (application_ != NULL) {
      auto application = Application::New(application_, runtime_);
      g_object_unref(application_);
      return application;
    } else {
      return Nan::Null();
    }
  }

  FridaApplication* application_;
};

NAN_METHOD(Device::GetFrontmostApplication) {
  HandleScope scope;

  auto isolate = info.GetIsolate();
  auto obj = info.Holder();
  auto wrapper = ObjectWrap::Unwrap<Device>(obj);

  auto operation = new GetFrontmostApplicationOperation();
  operation->Schedule(isolate, wrapper);

  info.GetReturnValue().Set(operation->GetPromise(isolate));
}

class EnumerateApplicationsOperation : public Operation<FridaDevice> {
 public:
  void Begin() {
    frida_device_enumerate_applications(handle_, OnReady, this);
  }

  void End(GAsyncResult* result, GError** error) {
    applications_ = frida_device_enumerate_applications_finish(handle_, result,
        error);
  }

  Local<Value> Result(Isolate* isolate) {
    auto size = frida_application_list_size(applications_);
    auto applications = Nan::New<v8::Array>(size);
    for (auto i = 0; i != size; i++) {
      auto handle = frida_application_list_get(applications_, i);
      auto application = Application::New(handle, runtime_);
      Nan::Set(applications, i, application);
      g_object_unref(handle);
    }

    g_object_unref(applications_);

    return applications;
  }

  FridaApplicationList* applications_;
};

NAN_METHOD(Device::EnumerateApplications) {
  HandleScope scope;

  auto isolate = info.GetIsolate();
  auto obj = info.Holder();
  auto wrapper = ObjectWrap::Unwrap<Device>(obj);

  auto operation = new EnumerateApplicationsOperation();
  operation->Schedule(isolate, wrapper);

  info.GetReturnValue().Set(operation->GetPromise(isolate));
}

class EnumerateProcessesOperation : public Operation<FridaDevice> {
 public:
  void Begin() {
    frida_device_enumerate_processes(handle_, OnReady, this);
  }

  void End(GAsyncResult* result, GError** error) {
    processes_ = frida_device_enumerate_processes_finish(handle_, result,
        error);
  }

  Local<Value> Result(Isolate* isolate) {
    auto size = frida_process_list_size(processes_);
    auto processes = Nan::New<v8::Array>(size);
    for (auto i = 0; i != size; i++) {
      auto handle = frida_process_list_get(processes_, i);
      auto process = Process::New(handle, runtime_);
      Nan::Set(processes, i, process);
      g_object_unref(handle);
    }

    g_object_unref(processes_);

    return processes;
  }

  FridaProcessList* processes_;
};

NAN_METHOD(Device::EnumerateProcesses) {
  HandleScope scope;

  auto isolate = info.GetIsolate();
  auto obj = info.Holder();
  auto wrapper = ObjectWrap::Unwrap<Device>(obj);

  auto operation = new EnumerateProcessesOperation();
  operation->Schedule(isolate, wrapper);

  info.GetReturnValue().Set(operation->GetPromise(isolate));
}

class SpawnOperation : public Operation<FridaDevice> {
 public:
  SpawnOperation(gchar* path, gchar** argv, gchar** envp)
    : path_(path),
      argv_(argv),
      envp_(envp) {
  }

  ~SpawnOperation() {
    g_strfreev(envp_);
    g_strfreev(argv_);
    g_free(path_);
  }

  void Begin() {
    frida_device_spawn(handle_, path_, argv_, g_strv_length(argv_),
        envp_, g_strv_length(envp_), OnReady, this);
  }

  void End(GAsyncResult* result, GError** error) {
    pid_ = frida_device_spawn_finish(handle_, result, error);
  }

  Local<Value> Result(Isolate* isolate) {
    return Nan::New<v8::Uint32>(pid_);
  }

  gchar* path_;
  gchar** argv_;
  gchar** envp_;
  guint pid_;
};

NAN_METHOD(Device::Spawn) {
  HandleScope scope;

  auto isolate = info.GetIsolate();
  auto obj = info.Holder();
  auto wrapper = ObjectWrap::Unwrap<Device>(obj);

  gchar** argv = NULL;
  if (info.Length() >= 1 && info[0]->IsArray()) {
    auto elements = Local<v8::Array>::Cast(info[0]);
    uint32_t length = elements->Length();
    argv = g_new0(gchar *, length + 1);
    for (uint32_t i = 0; i != length; i++) {
      auto element_value = Nan::Get(elements, i).ToLocalChecked();
      if (element_value->IsString()) {
        String::Utf8Value element(Local<String>::Cast(element_value));
        argv[i] = g_strdup(*element);
      } else {
        g_strfreev(argv);
        argv = NULL;
        break;
      }
    }
  }
  if (argv == NULL) {
    Nan::ThrowTypeError("Bad argument, expected argv as an array of strings");
    return;
  }

  gchar** envp = g_get_environ();

  gchar* path = g_strdup(argv[0]);

  auto operation = new SpawnOperation(path, argv, envp);
  operation->Schedule(isolate, wrapper);

  info.GetReturnValue().Set(operation->GetPromise(isolate));
}

class ResumeOperation : public Operation<FridaDevice> {
 public:
  ResumeOperation(guint pid) : pid_(pid) {
  }

  void Begin() {
    frida_device_resume(handle_, pid_, OnReady, this);
  }

  void End(GAsyncResult* result, GError** error) {
    frida_device_resume_finish(handle_, result, error);
  }

  Local<Value> Result(Isolate* isolate) {
    return Nan::Undefined();
  }

  const guint pid_;
};

NAN_METHOD(Device::Resume) {
  HandleScope scope;

  auto isolate = info.GetIsolate();
  auto obj = info.Holder();
  auto wrapper = ObjectWrap::Unwrap<Device>(obj);

  if (info.Length() < 1 || !info[0]->IsNumber()) {
    Nan::ThrowTypeError("Bad argument, expected pid");
    return;
  }
  auto pid = info[0]->ToInteger()->Value();
  if (pid <= 0) {
    Nan::ThrowTypeError("Bad argument, expected pid");
    return;
  }

  auto operation = new ResumeOperation(static_cast<guint>(pid));
  operation->Schedule(isolate, wrapper);

  info.GetReturnValue().Set(operation->GetPromise(isolate));
}

class KillOperation : public Operation<FridaDevice> {
 public:
  KillOperation(guint pid) : pid_(pid) {
  }

  void Begin() {
    frida_device_kill(handle_, pid_, OnReady, this);
  }

  void End(GAsyncResult* result, GError** error) {
    frida_device_kill_finish(handle_, result, error);
  }

  Local<Value> Result(Isolate* isolate) {
    return Nan::Undefined();
  }

  const guint pid_;
};

NAN_METHOD(Device::Kill) {
  HandleScope scope;

  auto isolate = info.GetIsolate();
  auto obj = info.Holder();
  auto wrapper = ObjectWrap::Unwrap<Device>(obj);

  if (info.Length() < 1 || !info[0]->IsNumber()) {
    Nan::ThrowTypeError("Bad argument, expected pid");
    return;
  }
  auto pid = info[0]->ToInteger()->Value();
  if (pid <= 0) {
    Nan::ThrowTypeError("Bad argument, expected pid");
    return;
  }

  auto operation = new KillOperation(static_cast<guint>(pid));
  operation->Schedule(isolate, wrapper);

  info.GetReturnValue().Set(operation->GetPromise(isolate));
}

class AttachOperation : public Operation<FridaDevice> {
 public:
  AttachOperation(guint pid) : pid_(pid) {
  }

  void Begin() {
    frida_device_attach(handle_, pid_, OnReady, this);
  }

  void End(GAsyncResult* result, GError** error) {
    session_ = frida_device_attach_finish(handle_, result, error);
  }

  Local<Value> Result(Isolate* isolate) {
    auto wrapper = Session::New(session_, runtime_);
    g_object_unref(session_);
    return wrapper;
  }

  const guint pid_;
  FridaSession* session_;
};

NAN_METHOD(Device::Attach) {
  HandleScope scope;

  auto isolate = info.GetIsolate();
  auto obj = info.Holder();
  auto wrapper = ObjectWrap::Unwrap<Device>(obj);

  if (info.Length() < 1 || !info[0]->IsNumber()) {
    Nan::ThrowTypeError("Bad argument, expected pid");
    return;
  }
  auto pid = info[0]->ToInteger()->Value();
  if (pid <= 0) {
    Nan::ThrowTypeError("Bad argument, expected pid");
    return;
  }

  auto operation = new AttachOperation(static_cast<guint>(pid));
  operation->Schedule(isolate, wrapper);

  info.GetReturnValue().Set(operation->GetPromise(isolate));
}

}
