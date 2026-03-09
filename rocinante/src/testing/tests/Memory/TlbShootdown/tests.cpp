/**
 * Copyright (C) 2026 Andrew S. Rightenburg
 * GPL-3.0-or-later
 */

#include <src/testing/test.h>

#include <src/memory/tlb_shootdown_state.h>

namespace Rocinante::Testing {

namespace {

struct RequestRecorder final {
	std::uint64_t handled_count = 0;
	Rocinante::Memory::TlbShootdown::Request last_request{};
	bool should_succeed = true;
};

static bool RecordHandledRequest(const Rocinante::Memory::TlbShootdown::Request& request, void* context) {
	auto* recorder = static_cast<RequestRecorder*>(context);
	if (!recorder) return false;
	if (!recorder->should_succeed) return false;

	recorder->handled_count++;
	recorder->last_request = request;
	return true;
}

static void Test_TlbShootdown_CpuMask_BasicSemantics(TestContext* ctx) {
	using Rocinante::Memory::TlbShootdown::CpuMask;

	CpuMask mask;
	ROCINANTE_EXPECT_TRUE(ctx, mask.IsEmpty());
	ROCINANTE_EXPECT_TRUE(ctx, mask.Add(0));
	ROCINANTE_EXPECT_TRUE(ctx, mask.Add(5));
	ROCINANTE_EXPECT_TRUE(ctx, mask.Contains(0));
	ROCINANTE_EXPECT_TRUE(ctx, !mask.Contains(1));
	ROCINANTE_EXPECT_TRUE(ctx, mask.Contains(5));
	ROCINANTE_EXPECT_TRUE(ctx, mask.Remove(0));
	ROCINANTE_EXPECT_TRUE(ctx, !mask.Contains(0));
	ROCINANTE_EXPECT_TRUE(ctx, !mask.Add(static_cast<std::uint32_t>(CpuMask::kMaxCpuCount)));

	const CpuMask single_core = CpuMask::ForCore(3);
	ROCINANTE_EXPECT_TRUE(ctx, single_core.Contains(3));
	ROCINANTE_EXPECT_EQ_U64(ctx, single_core.bits, (1ull << 3));
	ROCINANTE_EXPECT_TRUE(ctx, CpuMask::ForCore(static_cast<std::uint32_t>(CpuMask::kMaxCpuCount)).IsEmpty());
}

static void Test_TlbShootdown_State_GenerationAck_BasicSemantics(TestContext* ctx) {
	using Rocinante::Memory::TlbShootdown::CpuMask;
	using Rocinante::Memory::TlbShootdown::State;

	State state;
	state.Reset();

	ROCINANTE_EXPECT_TRUE(ctx, state.GetOnlineCpuMask().IsEmpty());
	ROCINANTE_EXPECT_TRUE(ctx, state.SetCpuOnline(0, true));
	ROCINANTE_EXPECT_TRUE(ctx, state.SetCpuOnline(2, true));
	ROCINANTE_EXPECT_TRUE(ctx, !state.SetCpuOnline(static_cast<std::uint32_t>(CpuMask::kMaxCpuCount), true));

	const CpuMask online_mask = state.GetOnlineCpuMask();
	ROCINANTE_EXPECT_TRUE(ctx, online_mask.Contains(0));
	ROCINANTE_EXPECT_TRUE(ctx, online_mask.Contains(2));
	ROCINANTE_EXPECT_TRUE(ctx, !online_mask.Contains(1));

	const std::uint64_t generation1 = state.AllocateGeneration();
	const std::uint64_t generation2 = state.AllocateGeneration();
	ROCINANTE_EXPECT_EQ_U64(ctx, generation1, 1);
	ROCINANTE_EXPECT_EQ_U64(ctx, generation2, 2);

	ROCINANTE_EXPECT_TRUE(ctx, !state.HaveAllTargetsAcknowledged(online_mask, generation2));

	state.RecordAcknowledgedGeneration(0, generation2);
	ROCINANTE_EXPECT_TRUE(ctx, !state.HaveAllTargetsAcknowledged(online_mask, generation2));

	// A later acknowledged generation must also satisfy an earlier wait.
	state.RecordAcknowledgedGeneration(2, generation2 + 1);
	ROCINANTE_EXPECT_TRUE(ctx, state.HaveAllTargetsAcknowledged(online_mask, generation2));
	ROCINANTE_EXPECT_EQ_U64(ctx, state.GetAcknowledgedGeneration(2), generation2 + 1);

	ROCINANTE_EXPECT_TRUE(ctx, state.SetCpuOnline(2, false));
	const CpuMask reduced_online_mask = state.GetOnlineCpuMask();
	ROCINANTE_EXPECT_TRUE(ctx, reduced_online_mask.Contains(0));
	ROCINANTE_EXPECT_TRUE(ctx, !reduced_online_mask.Contains(2));
	ROCINANTE_EXPECT_TRUE(ctx, state.HaveAllTargetsAcknowledged(reduced_online_mask, generation2));
	ROCINANTE_EXPECT_EQ_U64(ctx, state.GetAcknowledgedGeneration(static_cast<std::uint32_t>(CpuMask::kMaxCpuCount)), 0);
}

static void Test_TlbShootdown_State_RequestMailbox_BasicSemantics(TestContext* ctx) {
	using Rocinante::Memory::TlbShootdown::CpuMask;
	using Rocinante::Memory::TlbShootdown::Request;
	using Rocinante::Memory::TlbShootdown::RequestType;
	using Rocinante::Memory::TlbShootdown::State;

	State state;
	state.Reset();
	ROCINANTE_EXPECT_TRUE(ctx, state.SetCpuOnline(0, true));
	ROCINANTE_EXPECT_TRUE(ctx, state.SetCpuOnline(2, true));

	const Request page_request{
		.generation = state.AllocateGeneration(),
		.type = RequestType::InvalidatePage,
		.address_space_id = 7,
		.virtual_address_page_base = 0x0000000100004000ull,
	};
	ROCINANTE_EXPECT_TRUE(ctx, state.PublishRequestToTargets(state.GetOnlineCpuMask(), page_request));

	Request observed_request{};
	ROCINANTE_EXPECT_TRUE(ctx, state.TryReadPublishedRequestForCore(0, &observed_request));
	ROCINANTE_EXPECT_EQ_U64(ctx, observed_request.generation, page_request.generation);
	ROCINANTE_EXPECT_EQ_U64(ctx, static_cast<std::uint64_t>(observed_request.type), static_cast<std::uint64_t>(RequestType::InvalidatePage));
	ROCINANTE_EXPECT_EQ_U64(ctx, observed_request.address_space_id, page_request.address_space_id);
	ROCINANTE_EXPECT_EQ_U64(ctx, observed_request.virtual_address_page_base, page_request.virtual_address_page_base);

	Request observed_request_core2{};
	ROCINANTE_EXPECT_TRUE(ctx, state.TryReadPublishedRequestForCore(2, &observed_request_core2));
	ROCINANTE_EXPECT_EQ_U64(ctx, observed_request_core2.generation, page_request.generation);
	ROCINANTE_EXPECT_EQ_U64(ctx, observed_request_core2.address_space_id, page_request.address_space_id);

	Request no_request{};
	ROCINANTE_EXPECT_TRUE(ctx, !state.TryReadPublishedRequestForCore(1, &no_request));

	const Request invalid_page_request{
		.generation = state.AllocateGeneration(),
		.type = RequestType::InvalidatePage,
		.address_space_id = 9,
		.virtual_address_page_base = 0x0000000100004001ull,
	};
	ROCINANTE_EXPECT_TRUE(ctx, !state.PublishRequestToCore(0, invalid_page_request));

	const Request invalidate_asid_request{
		.generation = state.AllocateGeneration(),
		.type = RequestType::InvalidateAsid,
		.address_space_id = 11,
		.virtual_address_page_base = 0,
	};
	state.RecordAcknowledgedGeneration(0, page_request.generation);
	ROCINANTE_EXPECT_TRUE(ctx, state.PublishRequestToCore(0, invalidate_asid_request));
	ROCINANTE_EXPECT_TRUE(ctx, state.TryReadPublishedRequestForCore(0, &observed_request));
	ROCINANTE_EXPECT_EQ_U64(ctx, static_cast<std::uint64_t>(observed_request.type), static_cast<std::uint64_t>(RequestType::InvalidateAsid));
	ROCINANTE_EXPECT_EQ_U64(ctx, observed_request.address_space_id, invalidate_asid_request.address_space_id);
	ROCINANTE_EXPECT_EQ_U64(ctx, observed_request.virtual_address_page_base, 0);

	ROCINANTE_EXPECT_TRUE(ctx, !state.PublishRequestToCore(
		static_cast<std::uint32_t>(CpuMask::kMaxCpuCount),
		invalidate_asid_request));
	ROCINANTE_EXPECT_TRUE(ctx, !state.TryReadPublishedRequestForCore(0, nullptr));
}

static void Test_TlbShootdown_State_RequestHelpers_BasicSemantics(TestContext* ctx) {
	using Rocinante::Memory::TlbShootdown::CpuMask;
	using Rocinante::Memory::TlbShootdown::PublishedRequest;
	using Rocinante::Memory::TlbShootdown::Request;
	using Rocinante::Memory::TlbShootdown::RequestType;
	using Rocinante::Memory::TlbShootdown::State;

	State state;
	state.Reset();
	ROCINANTE_EXPECT_TRUE(ctx, state.SetCpuOnline(0, true));
	ROCINANTE_EXPECT_TRUE(ctx, state.SetCpuOnline(2, true));

	const CpuMask targets = state.GetOnlineCpuMask();
	Request request{};
	ROCINANTE_EXPECT_TRUE(ctx, state.PublishInvalidatePageRequestToTargets(
		targets,
		17,
		0x0000000100006000ull,
		&request));
	ROCINANTE_EXPECT_EQ_U64(ctx, request.generation, 1);
	ROCINANTE_EXPECT_EQ_U64(ctx, static_cast<std::uint64_t>(request.type), static_cast<std::uint64_t>(RequestType::InvalidatePage));
	ROCINANTE_EXPECT_TRUE(ctx, !state.IsRequestCompletedForTargets(targets, request));
	ROCINANTE_EXPECT_TRUE(ctx, !state.HaveAllTargetsAcknowledged(targets, request.generation));

	Request pending_request{};
	ROCINANTE_EXPECT_TRUE(ctx, state.TryReadPendingRequestForCore(0, &pending_request));
	ROCINANTE_EXPECT_EQ_U64(ctx, pending_request.generation, request.generation);
	ROCINANTE_EXPECT_TRUE(ctx, state.TryReadPendingRequestForCore(2, &pending_request));
	ROCINANTE_EXPECT_TRUE(ctx, !state.TryReadPendingRequestForCore(1, &pending_request));

	Request blocked_request{};
	ROCINANTE_EXPECT_TRUE(ctx, !state.PublishInvalidateAsidRequestToTargets(targets, 18, &blocked_request));

	RequestRecorder recorder{};
	ROCINANTE_EXPECT_TRUE(ctx, state.HandleAndAcknowledgePendingRequestForCore(0, &RecordHandledRequest, &recorder));
	ROCINANTE_EXPECT_EQ_U64(ctx, recorder.handled_count, 1);
	ROCINANTE_EXPECT_EQ_U64(ctx, recorder.last_request.address_space_id, 17);
	ROCINANTE_EXPECT_TRUE(ctx, !state.IsRequestCompletedForTargets(targets, request));
	ROCINANTE_EXPECT_TRUE(ctx, !state.HaveAllTargetsAcknowledged(targets, request.generation));
	ROCINANTE_EXPECT_TRUE(ctx, !state.TryReadPendingRequestForCore(0, &pending_request));

	recorder.should_succeed = false;
	ROCINANTE_EXPECT_TRUE(ctx, !state.HandleAndAcknowledgePendingRequestForCore(2, &RecordHandledRequest, &recorder));
	ROCINANTE_EXPECT_TRUE(ctx, state.TryReadPendingRequestForCore(2, &pending_request));
	ROCINANTE_EXPECT_TRUE(ctx, !state.HaveAllTargetsAcknowledged(targets, request.generation));

	recorder.should_succeed = true;
	ROCINANTE_EXPECT_TRUE(ctx, state.HandleAndAcknowledgePendingRequestForCore(2, &RecordHandledRequest, &recorder));
	ROCINANTE_EXPECT_EQ_U64(ctx, recorder.handled_count, 2);
	ROCINANTE_EXPECT_TRUE(ctx, state.IsRequestCompletedForTargets(targets, request));
	ROCINANTE_EXPECT_TRUE(ctx, state.HaveAllTargetsAcknowledged(targets, request.generation));
	ROCINANTE_EXPECT_TRUE(ctx, !state.IsRequestCompletedForTargets(targets, Request{}));

	Request asid_request{};
	ROCINANTE_EXPECT_TRUE(ctx, state.PublishInvalidateAsidRequestToTargets(targets, 99, &asid_request));
	ROCINANTE_EXPECT_EQ_U64(ctx, asid_request.generation, 2);
	ROCINANTE_EXPECT_EQ_U64(ctx, static_cast<std::uint64_t>(asid_request.type), static_cast<std::uint64_t>(RequestType::InvalidateAsid));
	ROCINANTE_EXPECT_TRUE(ctx, !state.PublishInvalidateGlobalAllRequestToTargets(targets, &blocked_request));

	Request global_request{};
	ROCINANTE_EXPECT_TRUE(ctx, state.HandleAndAcknowledgePendingRequestForCore(0, &RecordHandledRequest, &recorder));
	ROCINANTE_EXPECT_TRUE(ctx, state.HandleAndAcknowledgePendingRequestForCore(2, &RecordHandledRequest, &recorder));
	ROCINANTE_EXPECT_TRUE(ctx, state.PublishInvalidateGlobalAllRequestToTargets(targets, &global_request));
	ROCINANTE_EXPECT_EQ_U64(ctx, global_request.generation, 3);
	ROCINANTE_EXPECT_EQ_U64(ctx, static_cast<std::uint64_t>(global_request.type), static_cast<std::uint64_t>(RequestType::InvalidateGlobalAll));
	ROCINANTE_EXPECT_TRUE(ctx, !state.HandleAndAcknowledgePendingRequestForCore(1, &RecordHandledRequest, &recorder));
	ROCINANTE_EXPECT_TRUE(ctx, !state.HandleAndAcknowledgePendingRequestForCore(0, nullptr, &recorder));
}

static void Test_TlbShootdown_State_MaskSampling_BasicSemantics(TestContext* ctx) {
	using Rocinante::Memory::TlbShootdown::PublishedRequest;
	using Rocinante::Memory::TlbShootdown::State;

	State state;
	state.Reset();
	ROCINANTE_EXPECT_TRUE(ctx, state.SetCpuOnline(0, true));
	ROCINANTE_EXPECT_TRUE(ctx, state.SetCpuOnline(2, true));

	PublishedRequest published_request{};
	ROCINANTE_EXPECT_TRUE(ctx, state.PublishInvalidateAsidRequestToCurrentOnlineTargets(33, &published_request));
	ROCINANTE_EXPECT_TRUE(ctx, published_request.IsValid());
	ROCINANTE_EXPECT_TRUE(ctx, published_request.target_cpu_mask.Contains(0));
	ROCINANTE_EXPECT_TRUE(ctx, published_request.target_cpu_mask.Contains(2));
	ROCINANTE_EXPECT_TRUE(ctx, !published_request.target_cpu_mask.Contains(3));
	ROCINANTE_EXPECT_EQ_U64(ctx, published_request.request.generation, 1);

	ROCINANTE_EXPECT_TRUE(ctx, state.SetCpuOnline(3, true));
	ROCINANTE_EXPECT_TRUE(ctx, state.SetCpuOnline(2, false));
	ROCINANTE_EXPECT_TRUE(ctx, !state.IsPublishedRequestCompleted(published_request));
	ROCINANTE_EXPECT_TRUE(ctx, !state.HaveAllTargetsAcknowledged(state.GetOnlineCpuMask(), published_request.request.generation));

	state.RecordAcknowledgedGeneration(0, published_request.request.generation);
	ROCINANTE_EXPECT_TRUE(ctx, !state.IsPublishedRequestCompleted(published_request));

	state.RecordAcknowledgedGeneration(2, published_request.request.generation);
	ROCINANTE_EXPECT_TRUE(ctx, state.IsPublishedRequestCompleted(published_request));
	ROCINANTE_EXPECT_TRUE(ctx, !state.HaveAllTargetsAcknowledged(state.GetOnlineCpuMask(), published_request.request.generation));
	ROCINANTE_EXPECT_TRUE(ctx, !state.IsPublishedRequestCompleted(PublishedRequest{}));
	ROCINANTE_EXPECT_TRUE(ctx, !published_request.target_cpu_mask.Contains(3));
	ROCINANTE_EXPECT_TRUE(ctx, published_request.target_cpu_mask.Contains(2));
}

} // namespace

void TestEntry_TlbShootdown_CpuMask_BasicSemantics(TestContext* ctx) {
	Test_TlbShootdown_CpuMask_BasicSemantics(ctx);
}

void TestEntry_TlbShootdown_State_GenerationAck_BasicSemantics(TestContext* ctx) {
	Test_TlbShootdown_State_GenerationAck_BasicSemantics(ctx);
}

void TestEntry_TlbShootdown_State_RequestMailbox_BasicSemantics(TestContext* ctx) {
	Test_TlbShootdown_State_RequestMailbox_BasicSemantics(ctx);
}

void TestEntry_TlbShootdown_State_RequestHelpers_BasicSemantics(TestContext* ctx) {
	Test_TlbShootdown_State_RequestHelpers_BasicSemantics(ctx);
}

void TestEntry_TlbShootdown_State_MaskSampling_BasicSemantics(TestContext* ctx) {
	Test_TlbShootdown_State_MaskSampling_BasicSemantics(ctx);
}

} // namespace Rocinante::Testing
