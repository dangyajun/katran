/* Copyright (C) 2018-present, Facebook, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "katran/lib/testing/BpfTester.h"

#include <fmt/core.h>
#include <folly/String.h>
#include <folly/io/IOBuf.h>
#include <glog/logging.h>
#include <iostream>

namespace katran {

namespace {
// xdp works in 1 page per packet mode. on x86 it's 4k
constexpr uint64_t kMaxXdpPcktSize = 4096;
constexpr int kTestRepeatCount = 1;
std::unordered_map<int, std::string> kXdpCodes{
    {0, "XDP_ABORTED"},
    {1, "XDP_DROP"},
    {2, "XDP_PASS"},
    {3, "XDP_TX"},
};
std::unordered_map<int, std::string> kTcCodes{
    {-1, "TC_ACT_UNSPEC"},
    {0, "TC_ACT_OK"},
    {1, "TC_ACT_RECLASSIFY"},
    {2, "TC_ACT_SHOT"},
    {3, "TC_ACT_PIPE"},
    {4, "TC_ACT_STOLEN"},
    {5, "TC_ACT_QUEUED"},
    {6, "TC_ACT_REPEAT"},
    {7, "TC_ACT_REDIRECT"},
};

constexpr uint32_t kNanosecInSec = 1000000000;
} // namespace

BpfTester::BpfTester(const TesterConfig& config)
    : config_(config),
      parser_(config.inputFileName, config.outputFileName),
      adapter_(false /* set_limits */) {}

void BpfTester::printPcktBase64() {
  if (config_.inputFileName.empty()) {
    LOG(INFO) << "can't print packet. no input pcap file specified";
    return;
  }
  std::string pckt;
  while (true) {
    pckt = parser_.getPacketFromPcapBase64();
    if (pckt.empty()) {
      VLOG(2) << "we have read all the packets from pcap file";
      break;
    }
    std::cout << pckt << std::endl;
  }
}

void BpfTester::writePcapOutput(std::unique_ptr<folly::IOBuf>&& buf) {
  if (config_.outputFileName.empty()) {
    VLOG(2) << "no output file specified";
    return;
  }
  auto success = parser_.writePacket(std::move(buf));
  if (!success) {
    LOG(INFO) << "failed to write pckt into output "
              << "pcap file: " << config_.outputFileName;
  }
}

void BpfTester::testPcktsFromPcap() {
  if (config_.inputFileName.empty() || config_.bpfProgFd < 0) {
    LOG(INFO) << "can't run pcap based tests. input pcap file or bpf prog fd "
              << "aren't specified";
    return;
  }

  uint32_t output_pckt_size{0};
  uint32_t prog_ret_val{0};
  uint64_t pckt_num{1};
  while (true) {
    auto buf = folly::IOBuf::create(kMaxXdpPcktSize);
    auto pckt = parser_.getPacketFromPcap();
    if (pckt == nullptr) {
      VLOG(2) << "we have read all the packets from pcap file";
      break;
    }
    auto res = adapter_.testXdpProg(
        config_.bpfProgFd,
        kTestRepeatCount,
        pckt->writableData(),
        pckt->length(),
        buf->writableData(),
        &output_pckt_size,
        &prog_ret_val);
    if (res < 0) {
      LOG(INFO) << "failed to run bpf test on pckt #" << pckt_num;
      ++pckt_num;
      continue;
    }
    if (prog_ret_val > 3) {
      LOG(INFO) << "unsupported return value: " << prog_ret_val;
    } else {
      LOG(INFO) << "xdp run's result from pckt #" << pckt_num << " is "
                << kXdpCodes[prog_ret_val];
    }
    // adjust IOBuf so data data_end will acount for writen data
    buf->append(output_pckt_size);
    writePcapOutput(buf->cloneOne());
    ++pckt_num;
  }
}

bool BpfTester::testFromFixture() {
  return runBpfTesterFromFixtures(config_.bpfProgFd, kXdpCodes, {});
}

void BpfTester::testClsFromFixture(
    int progFd,
    std::vector<struct __sk_buff> ctxs_in) {
  std::vector<void*> ctxs;
  for (auto& ctx : ctxs_in) {
    ctxs.push_back(&ctx);
  }
  runBpfTesterFromFixtures(progFd, kTcCodes, ctxs, sizeof(struct __sk_buff));
}

bool BpfTester::runBpfTesterFromFixtures(
    int progFd,
    std::unordered_map<int, std::string> retvalTranslation,
    std::vector<void*> ctxs_in,
    uint32_t ctx_size) {
  if (ctxs_in.size() != 0) {
    if (ctx_size == 0) {
      LOG(INFO)
          << "size of single ctx value must be non zero if ctxs are specified";
      return false;
    }
    if (ctxs_in.size() != config_.testData.size()) {
      LOG(INFO) << "ctxs and input datasets must have equal number of elements";
      return false;
    }
  }

  uint32_t output_pckt_size{0};
  uint32_t prog_ret_val{0};
  uint64_t pckt_num{1};
  std::string ret_val_str;
  std::string test_result;
  uint64_t packetsRoutedGlobalLruBefore{0};
  uint64_t packetsRoutedGlobalLruAfter{0};
  bool overallSuccess{true};
  for (int i = 0; i < config_.testData.size(); i++) {
    bool iterationSuccess = true;

    if (config_.singleTestRunPacketNumber_ &&
        *config_.singleTestRunPacketNumber_ != (i + 1)) {
      ++pckt_num;
      VLOG(2) << "Skipped test for packet #" << i;
      continue;
    }
    void* ctx_in = ctxs_in.size() != 0 ? ctxs_in[i] : nullptr;
    auto pckt_buf = folly::IOBuf::create(kMaxXdpPcktSize);
    auto input_pckt =
        parser_.getPacketFromBase64(config_.testData[i].inputPacket);
    writePcapOutput(input_pckt->cloneOne());
    if (config_.testData[i].routedThroughGlobalLru) {
      packetsRoutedGlobalLruBefore = getGlobalLruRoutedPackets();
    }
    VLOG(2) << "Running test for pckt #" << pckt_num
            << " with description: " << config_.testData[i].description;
    auto res = adapter_.testXdpProg(
        progFd,
        kTestRepeatCount,
        input_pckt->writableData(),
        input_pckt->length(),
        pckt_buf->writableData(),
        &output_pckt_size,
        &prog_ret_val,
        nullptr, // duration
        ctx_in,
        ctx_size);
    if (res < 0) {
      LOG(INFO) << "failed to run bpf test on pckt #" << pckt_num << " errno "
                << errno << " : " << folly::errnoStr(errno);
      ++pckt_num;
      overallSuccess = false;
      continue;
    }
    if (config_.testData[i].routedThroughGlobalLru) {
      packetsRoutedGlobalLruAfter = getGlobalLruRoutedPackets();
    }
    bool packetRoutedThroughGlobalLru =
        ((packetsRoutedGlobalLruAfter - packetsRoutedGlobalLruBefore) == 1);

    auto ret_val_iter = retvalTranslation.find(prog_ret_val);
    if (ret_val_iter == retvalTranslation.end()) {
      ret_val_str = "UNKNOWN";
    } else {
      ret_val_str = ret_val_iter->second;
    }
    // adjust IOBuf so data data_end will acount for writen data
    pckt_buf->append(output_pckt_size);
    writePcapOutput(pckt_buf->cloneOne());

    if (ret_val_str != config_.testData[i].expectedReturnValue) {
      VLOG(2) << "value from test: " << ret_val_str
              << " expected: " << config_.testData[i].expectedReturnValue;
      test_result = "\033[31mFailed\033[0m";
      auto output_test_pckt =
          parser_.convertPacketToBase64(std::move(pckt_buf));
      if (output_test_pckt != config_.testData[i].expectedOutputPacket) {
        VLOG(2) << "output packet not equal to expected one; expected pkt="
                << config_.testData[i].expectedOutputPacket
                << ", actual=" << output_test_pckt;
      }
      iterationSuccess = false;
    }

    if (iterationSuccess && config_.testData[i].routedThroughGlobalLru) {
      if (*config_.testData[i].routedThroughGlobalLru &&
          !packetRoutedThroughGlobalLru) {
        VLOG(2)
            << "packet should have been routed through global lru, but wasn't";
        test_result = "\033[31mFailed\033[0m";
        iterationSuccess = false;
      } else if (
          !*config_.testData[i].routedThroughGlobalLru &&
          packetRoutedThroughGlobalLru) {
        VLOG(2)
            << "packet should not have been routed through global lru, but was";
        test_result = "\033[31mFailed\033[0m";
        iterationSuccess = false;
      }
    }

    if (iterationSuccess) {
      test_result = "\033[32mPassed\033[0m";
      auto output_test_pckt =
          parser_.convertPacketToBase64(std::move(pckt_buf));
      if (output_test_pckt != config_.testData[i].expectedOutputPacket) {
        VLOG(2) << "output packet not equal to expected one; expected pkt="
                << config_.testData[i].expectedOutputPacket
                << ", actual=" << output_test_pckt;
        test_result = "\033[31mFailed\033[0m";
        iterationSuccess = false;
      }
    }

    overallSuccess = overallSuccess && iterationSuccess;

    VLOG(2) << "pckt #" << pckt_num;
    LOG(INFO) << fmt::format(
        "Test: {:60} result: {}", config_.testData[i].description, test_result);
    ++pckt_num;
  }
  return overallSuccess;
}

void BpfTester::resetTestFixtures(const std::vector<PacketAttributes>& data) {
  config_.testData = data;
}

void BpfTester::testPerfFromFixture(uint32_t repeat, const int position) {
  // for inputData format is <pckt_base64, test description>
  int first_index{0}, last_index{0};
  uint32_t duration{0};
  uint64_t pckt_num{1};
  std::string ret_val_str;
  std::string test_result;
  if (position < 0 || position >= config_.testData.size()) {
    first_index = 0;
    last_index = config_.testData.size();
  } else {
    first_index = position;
    last_index = first_index + 1;
  }
  for (int i = first_index; i < last_index; i++) {
    auto buf = folly::IOBuf::create(kMaxXdpPcktSize);
    auto input_pckt =
        parser_.getPacketFromBase64(config_.testData[i].inputPacket);
    auto res = adapter_.testXdpProg(
        config_.bpfProgFd,
        repeat,
        input_pckt->writableData(),
        input_pckt->length(),
        buf->writableData(),
        nullptr, // output pckt size
        nullptr, // retval
        &duration);
    if (res < 0) {
      LOG(INFO) << "failed to run bpf test on pckt #" << pckt_num;
      ++pckt_num;
      continue;
    }
    VLOG(2) << "pckt #" << pckt_num;
    if (duration == 0) {
      duration = 1;
    }
    auto pps = kNanosecInSec / duration;
    LOG(INFO) << fmt::format(
        "Test: {:60} duration: {:10} ns/pckt or {} pps",
        config_.testData[i].description,
        duration,
        pps);
    ++pckt_num;
  }
}

uint64_t BpfTester::getGlobalLruRoutedPackets() {
  auto globalLruStats = katranLb_->getGlobalLruStats();
  return globalLruStats.v2;
}

} // namespace katran
