/*
 * Copyright (C) 2018 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "src/trace_processor/importers/systrace/systrace_trace_parser.h"

#include "perfetto/ext/base/string_splitter.h"
#include "perfetto/ext/base/string_utils.h"
#include "src/trace_processor/args_tracker.h"
#include "src/trace_processor/event_tracker.h"
#include "src/trace_processor/importers/ftrace/sched_event_tracker.h"
#include "src/trace_processor/importers/systrace/systrace_parser.h"
#include "src/trace_processor/process_tracker.h"
#include "src/trace_processor/slice_tracker.h"

#include <inttypes.h>
#include <string>
#include <unordered_map>

namespace perfetto {
namespace trace_processor {

namespace {

std::string SubstrTrim(const std::string& input, size_t start, size_t end) {
  std::string s = input.substr(start, end - start);
  s.erase(s.begin(), std::find_if(s.begin(), s.end(),
                                  [](int ch) { return !std::isspace(ch); }));
  s.erase(std::find_if(s.rbegin(), s.rend(),
                       [](int ch) { return !std::isspace(ch); })
              .base(),
          s.end());
  return s;
}

std::pair<size_t, size_t> FindTask(const std::string& line) {
  size_t start;
  for (start = 0; start < line.size() && isspace(line[start]); ++start)
    ;
  size_t length;
  for (length = 0; start + length < line.size() && line[start + length] != '-';
       ++length)
    ;
  return std::pair<size_t, size_t>(start, length);
}

}  // namespace

SystraceTraceParser::SystraceTraceParser(TraceProcessorContext* ctx)
    : context_(ctx),
      sched_wakeup_name_id_(ctx->storage->InternString("sched_wakeup")),
      cpu_idle_name_id_(ctx->storage->InternString("cpuidle")) {}
SystraceTraceParser::~SystraceTraceParser() = default;

util::Status SystraceTraceParser::Parse(std::unique_ptr<uint8_t[]> owned_buf,
                                        size_t size) {
  if (state_ == ParseState::kEndOfSystrace)
    return util::OkStatus();
  partial_buf_.insert(partial_buf_.end(), &owned_buf[0], &owned_buf[size]);

  if (state_ == ParseState::kBeforeParse) {
    state_ = partial_buf_[0] == '<' ? ParseState::kHtmlBeforeSystrace
                                    : ParseState::kSystrace;
  }

  const char kSystraceStart[] =
      R"(<script class="trace-data" type="application/text">)";
  auto start_it = partial_buf_.begin();
  for (;;) {
    auto line_it = std::find(start_it, partial_buf_.end(), '\n');
    if (line_it == partial_buf_.end())
      break;

    std::string buffer(start_it, line_it);
    if (state_ == ParseState::kHtmlBeforeSystrace) {
      if (base::Contains(buffer, kSystraceStart)) {
        state_ = ParseState::kSystrace;
      }
    } else if (state_ == ParseState::kSystrace) {
      if (base::Contains(buffer, R"(</script>)")) {
        state_ = kEndOfSystrace;
        break;
      } else if (!base::StartsWith(buffer, "#")) {
        ParseSingleSystraceEvent(buffer);
      }
    }
    start_it = line_it + 1;
  }
  if (state_ == ParseState::kEndOfSystrace) {
    partial_buf_.clear();
  } else {
    partial_buf_.erase(partial_buf_.begin(), start_it);
  }
  return util::OkStatus();
}

// TODO(hjd): This should be more robust to being passed random input.
// This can happen if we mess up detecting a gzip trace for example.
util::Status SystraceTraceParser::ParseSingleSystraceEvent(
    const std::string& buffer) {
  // An example line from buffer looks something like the following:
  // <idle>-0     (-----) [000] d..1 16500.715638: cpu_idle: state=0 cpu_id=0
  //
  // However, sometimes the tgid can be missing and buffer looks like this:
  // <idle>-0     [000] ...2     0.002188: task_newtask: pid=1 ...

  size_t task_start;
  size_t task_length;
  std::tie<size_t, size_t>(task_start, task_length) = FindTask(buffer);

  size_t task_idx = task_start + task_length;
  std::string task = buffer.substr(task_start, task_length);

  // Try and figure out whether tgid is present by searching for '(' but only
  // if it occurs before the start of cpu (indiciated by '[') - this is because
  // '(' can also occur in the args of an event.
  auto tgid_idx = buffer.find('(', task_idx + 1);
  auto cpu_idx = buffer.find('[', task_idx + 1);
  bool has_tgid = tgid_idx != std::string::npos && tgid_idx < cpu_idx;

  if (cpu_idx == std::string::npos) {
    return util::Status("Could not find [ in " + buffer);
  }

  auto pid_end = has_tgid ? cpu_idx : tgid_idx;
  std::string pid_str = SubstrTrim(buffer, task_idx + 1, pid_end);
  base::Optional<uint32_t> maybe_pid = base::StringToUInt32(pid_str);
  if (!maybe_pid.has_value()) {
    return util::Status("Could not convert pid " + pid_str);
  }
  uint32_t pid = maybe_pid.value();
  context_->process_tracker->GetOrCreateThread(pid);

  if (has_tgid) {
    auto tgid_end = buffer.find(')', tgid_idx + 1);
    std::string tgid_str = SubstrTrim(buffer, tgid_idx + 1, tgid_end);
    base::Optional<uint32_t> tgid = base::StringToUInt32(tgid_str);
    if (tgid) {
      context_->process_tracker->UpdateThread(pid, tgid.value());
    }
  }

  auto cpu_end = buffer.find(']', cpu_idx + 1);
  std::string cpu_str = SubstrTrim(buffer, cpu_idx + 1, cpu_end);
  base::Optional<uint32_t> maybe_cpu = base::StringToUInt32(cpu_str);
  if (!maybe_cpu.has_value()) {
    return util::Status("Could not convert cpu " + cpu_str);
  }
  uint32_t cpu = maybe_cpu.value();

  auto ts_idx = buffer.find(' ', cpu_end + 2);
  auto ts_end = buffer.find(':', ts_idx + 1);
  std::string ts_str = SubstrTrim(buffer, ts_idx + 1, ts_end);
  base::Optional<double> maybe_ts = base::StringToDouble(ts_str);
  if (!maybe_ts.has_value()) {
    return util::Status("Could not convert ts");
  }
  int64_t ts = static_cast<int64_t>(maybe_ts.value() * 1e9);

  auto fn_idx = buffer.find(':', ts_end + 2);
  std::string fn = SubstrTrim(buffer, ts_end + 2, fn_idx);

  std::string args_str = SubstrTrim(buffer, fn_idx + 2, buffer.size());

  std::unordered_map<std::string, std::string> args;
  for (base::StringSplitter ss(args_str.c_str(), ' '); ss.Next();) {
    std::string key;
    std::string value;
    for (base::StringSplitter inner(ss.cur_token(), '='); inner.Next();) {
      if (key.empty()) {
        key = inner.cur_token();
      } else {
        value = inner.cur_token();
      }
    }
    args.emplace(std::move(key), std::move(value));
  }
  if (fn == "sched_switch") {
    auto prev_state_str = args["prev_state"];
    int64_t prev_state =
        ftrace_utils::TaskState(prev_state_str.c_str()).raw_state();

    auto prev_pid = base::StringToUInt32(args["prev_pid"]);
    auto prev_comm = base::StringView(args["prev_comm"]);
    auto prev_prio = base::StringToInt32(args["prev_prio"]);
    auto next_pid = base::StringToUInt32(args["next_pid"]);
    auto next_comm = base::StringView(args["next_comm"]);
    auto next_prio = base::StringToInt32(args["next_prio"]);

    if (!(prev_pid.has_value() && prev_prio.has_value() &&
          next_pid.has_value() && next_prio.has_value())) {
      return util::Status("Could not parse sched_switch");
    }

    context_->sched_tracker->PushSchedSwitch(
        cpu, ts, prev_pid.value(), prev_comm, prev_prio.value(), prev_state,
        next_pid.value(), next_comm, next_prio.value());
  } else if (fn == "tracing_mark_write" || fn == "0" || fn == "print") {
    context_->systrace_parser->ParsePrintEvent(ts, pid, args_str.c_str());
  } else if (fn == "sched_wakeup") {
    auto comm = args["comm"];
    base::Optional<uint32_t> wakee_pid = base::StringToUInt32(args["pid"]);
    if (!wakee_pid.has_value()) {
      return util::Status("Could not convert wakee_pid");
    }

    StringId name_id = context_->storage->InternString(base::StringView(comm));
    auto wakee_utid =
        context_->process_tracker->UpdateThreadName(wakee_pid.value(), name_id);
    context_->event_tracker->PushInstant(ts, sched_wakeup_name_id_,
                                         0 /* value */, wakee_utid,
                                         RefType::kRefUtid);
  } else if (fn == "cpu_idle") {
    base::Optional<uint32_t> event_cpu = base::StringToUInt32(args["cpu_id"]);
    base::Optional<double> new_state = base::StringToDouble(args["state"]);
    if (!event_cpu.has_value()) {
      return util::Status("Could not convert event cpu");
    }
    if (!event_cpu.has_value()) {
      return util::Status("Could not convert state");
    }
    context_->event_tracker->PushCounter(ts, new_state.value(),
                                         cpu_idle_name_id_, event_cpu.value(),
                                         RefType::kRefCpuId);
  }

  return util::OkStatus();
}

}  // namespace trace_processor
}  // namespace perfetto
