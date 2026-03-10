/**
 * Copyright (C) 2026 Andrew S. Rightenburg
 * GPL-3.0-or-later
 */

#include <src/testing/test.h>

namespace Rocinante::Testing {

void TestEntry_CPUCFG_FakeBackend_DecodesWord1(TestContext* ctx);
void TestEntry_CPUCFG_FakeBackend_CachesWords(TestContext* ctx);
void TestEntry_CPUID_CoreId_IsReadableAndStable(TestContext* ctx);
void TestEntry_Atomics_FetchAddU64Db_BasicSemantics(TestContext* ctx);
void TestEntry_Atomics_FetchAddU64AcqRel_BasicSemantics(TestContext* ctx);
void TestEntry_Atomics_ExchangeU64Db_BasicSemantics(TestContext* ctx);
void TestEntry_Atomics_CompareExchangeU64Db_BasicSemantics(TestContext* ctx);
void TestEntry_Atomics_LoadStoreWrappers_BasicSemantics(TestContext* ctx);
void TestEntry_Traps_BREAK_EntersAndReturns(TestContext* ctx);
void TestEntry_Traps_INE_UndefinedInstruction_IsObserved(TestContext* ctx);
void TestEntry_Interrupts_TimerIRQ_DeliversAndClears(TestContext* ctx);

void TestEntry_Paging_MapTranslateUnmap(TestContext* ctx);
void TestEntry_Paging_RespectsVALENAndPALEN(TestContext* ctx);
void TestEntry_Paging_Physmap_MapsRootPageTableAndAttributes(TestContext* ctx);
void TestEntry_Paging_UnmapReclaimsIntermediateTables(TestContext* ctx);
void TestEntry_Paging_MapCount_TracksLeafMappings(TestContext* ctx);
void TestEntry_AddressSpace_DestroyPageTables_FreesRootAndSubtables(TestContext* ctx);

void TestEntry_KernelVirtualAddressAllocator_AllocateFreeCoalesce(TestContext* ctx);
void TestEntry_KernelVirtualAddressAllocator_ReserveCarvesFixedRange(TestContext* ctx);

void TestEntry_KernelMappings_MapTranslateUnmapAndFree(TestContext* ctx);
void TestEntry_KernelMappings_MapNewRange4KiB(TestContext* ctx);
void TestEntry_KernelMappings_MapNewRange4KiB_ReclaimsBackingPages(TestContext* ctx);
void TestEntry_KernelMappings_MapNewGuardedRange4KiB(TestContext* ctx);
void TestEntry_KernelMappings_IoremapMmio4KiB(TestContext* ctx);
void TestEntry_TlbShootdown_CpuMask_BasicSemantics(TestContext* ctx);
void TestEntry_TlbShootdown_State_GenerationAck_BasicSemantics(TestContext* ctx);
void TestEntry_TlbShootdown_State_OnlineMaskFreeze_BasicSemantics(TestContext* ctx);
void TestEntry_TlbShootdown_State_RequestMailbox_BasicSemantics(TestContext* ctx);
void TestEntry_TlbShootdown_State_RequestHelpers_BasicSemantics(TestContext* ctx);
void TestEntry_TlbShootdown_State_MaskSampling_BasicSemantics(TestContext* ctx);

void TestEntry_VMM_VMA_InsertLookup(TestContext* ctx);
void TestEntry_VMM_AnonymousVmObject_Ownership(TestContext* ctx);
void TestEntry_VMM_UnmapVma4KiB_ReleasesAnonymousFrames(TestContext* ctx);

void TestEntry_PMM_RespectsReservedKernelAndDTB(TestContext* ctx);
void TestEntry_PMM_DoesNotClobberReservedDuringBitmapPlacement(TestContext* ctx);
void TestEntry_PMM_ClampsTrackedRangeToPALEN(TestContext* ctx);
void TestEntry_PMM_BitmapPlacement_RespectsPALEN(TestContext* ctx);
void TestEntry_PMM_Initialize_SingleUsableRegionContainingKernelAndDTB(TestContext* ctx);
void TestEntry_PMM_PageFrameNumberConversions(TestContext* ctx);
void TestEntry_PMM_ReferenceCount_RetainRelease(TestContext* ctx);

void TestEntry_PagingHw_EnablePaging_TlbRefillSmoke(TestContext* ctx);
void TestEntry_PagingHw_UnmappedAccess_FaultsAndReportsBadV(TestContext* ctx);
void TestEntry_PagingHw_PagingFaultObserver_DispatchesAndCanHandle(TestContext* ctx);
void TestEntry_PagingHw_PagingFaultObserver_MapsAndRetries(TestContext* ctx);
void TestEntry_PagingHw_KernelPager_MapsAndRetries(TestContext* ctx);
void TestEntry_PagingHw_VmmPager_MapsViaVmaAndVmObject(TestContext* ctx);
void TestEntry_PagingHw_ReadOnlyStore_RaisesPme(TestContext* ctx);
void TestEntry_PagingHw_NonExecutableFetch_RaisesPnx(TestContext* ctx);
void TestEntry_PagingHw_PostPaging_MapUnmap_Faults(TestContext* ctx);
void TestEntry_PagingHw_HigherHalfStack_GuardPageFaults(TestContext* ctx);
void TestEntry_PagingHw_AddressSpaces_SwitchPgdlChangesTranslation(TestContext* ctx);

extern const TestCase g_test_cases[] = {
	{"CPUCFG.FakeBackend.DecodesWord1", &TestEntry_CPUCFG_FakeBackend_DecodesWord1},
	{"CPUCFG.FakeBackend.CachesWords", &TestEntry_CPUCFG_FakeBackend_CachesWords},
	{"CPU.CPUID.CoreId.IsReadableAndStable", &TestEntry_CPUID_CoreId_IsReadableAndStable},
	{"CPU.Atomics.FetchAddU64Db.BasicSemantics", &TestEntry_Atomics_FetchAddU64Db_BasicSemantics},
	{"CPU.Atomics.FetchAddU64AcqRel.BasicSemantics", &TestEntry_Atomics_FetchAddU64AcqRel_BasicSemantics},
	{"CPU.Atomics.ExchangeU64Db.BasicSemantics", &TestEntry_Atomics_ExchangeU64Db_BasicSemantics},
	{"CPU.Atomics.CompareExchangeU64Db.BasicSemantics", &TestEntry_Atomics_CompareExchangeU64Db_BasicSemantics},
	{"CPU.Atomics.LoadStoreWrappers.BasicSemantics", &TestEntry_Atomics_LoadStoreWrappers_BasicSemantics},
	{"Traps.BREAK.EntersAndReturns", &TestEntry_Traps_BREAK_EntersAndReturns},
	{"Traps.INE.UndefinedInstruction.IsObserved", &TestEntry_Traps_INE_UndefinedInstruction_IsObserved},
	{"Interrupts.TimerIRQ.DeliversAndClears", &TestEntry_Interrupts_TimerIRQ_DeliversAndClears},
	{"Memory.Paging.MapTranslateUnmap", &TestEntry_Paging_MapTranslateUnmap},
	{"Memory.Paging.MapCount.TracksLeafMappings", &TestEntry_Paging_MapCount_TracksLeafMappings},
	{"Memory.Paging.RespectsVALENAndPALEN", &TestEntry_Paging_RespectsVALENAndPALEN},
	{"Memory.Paging.Physmap.MapsRootAndAttributes", &TestEntry_Paging_Physmap_MapsRootPageTableAndAttributes},
	{"Memory.Paging.UnmapReclaimsIntermediateTables", &TestEntry_Paging_UnmapReclaimsIntermediateTables},
	{"Memory.AddressSpace.DestroyPageTables.FreesRootAndSubtables", &TestEntry_AddressSpace_DestroyPageTables_FreesRootAndSubtables},
	{"Memory.KernelVirtualAddressAllocator.AllocateFreeCoalesce", &TestEntry_KernelVirtualAddressAllocator_AllocateFreeCoalesce},
	{"Memory.KernelVirtualAddressAllocator.ReserveCarvesFixedRange", &TestEntry_KernelVirtualAddressAllocator_ReserveCarvesFixedRange},
	{"Memory.KernelMappings.MapTranslateUnmapAndFree", &TestEntry_KernelMappings_MapTranslateUnmapAndFree},
	{"Memory.KernelMappings.MapNewRange4KiB", &TestEntry_KernelMappings_MapNewRange4KiB},
	{"Memory.KernelMappings.MapNewRange4KiB.ReclaimsBackingPages", &TestEntry_KernelMappings_MapNewRange4KiB_ReclaimsBackingPages},
	{"Memory.KernelMappings.MapNewGuardedRange4KiB", &TestEntry_KernelMappings_MapNewGuardedRange4KiB},
	{"Memory.KernelMappings.IoremapMmio4KiB", &TestEntry_KernelMappings_IoremapMmio4KiB},
	{"Memory.TlbShootdown.CpuMask.BasicSemantics", &TestEntry_TlbShootdown_CpuMask_BasicSemantics},
	{"Memory.TlbShootdown.State.GenerationAck.BasicSemantics", &TestEntry_TlbShootdown_State_GenerationAck_BasicSemantics},
	{"Memory.TlbShootdown.State.OnlineMaskFreeze.BasicSemantics", &TestEntry_TlbShootdown_State_OnlineMaskFreeze_BasicSemantics},
	{"Memory.TlbShootdown.State.RequestMailbox.BasicSemantics", &TestEntry_TlbShootdown_State_RequestMailbox_BasicSemantics},
	{"Memory.TlbShootdown.State.RequestHelpers.BasicSemantics", &TestEntry_TlbShootdown_State_RequestHelpers_BasicSemantics},
	{"Memory.TlbShootdown.State.MaskSampling.BasicSemantics", &TestEntry_TlbShootdown_State_MaskSampling_BasicSemantics},
	{"Memory.VMM.VMA.InsertLookup", &TestEntry_VMM_VMA_InsertLookup},
	{"Memory.VMM.AnonymousVmObject.Ownership", &TestEntry_VMM_AnonymousVmObject_Ownership},
	{"Memory.VMM.UnmapVma4KiB.ReleasesAnonymousFrames", &TestEntry_VMM_UnmapVma4KiB_ReleasesAnonymousFrames},
	{"Memory.PMM.RespectsReservedKernelAndDTB", &TestEntry_PMM_RespectsReservedKernelAndDTB},
	{"Memory.PMM.BitmapPlacement.DoesNotClobberReserved", &TestEntry_PMM_DoesNotClobberReservedDuringBitmapPlacement},
	{"Memory.PMM.ClampsTrackedRangeToPALEN", &TestEntry_PMM_ClampsTrackedRangeToPALEN},
	{"Memory.PMM.BitmapPlacement.RespectsPALEN", &TestEntry_PMM_BitmapPlacement_RespectsPALEN},
	{"Memory.PMM.Initialize.SingleUsableRegionContainingKernelAndDTB", &TestEntry_PMM_Initialize_SingleUsableRegionContainingKernelAndDTB},
	{"Memory.PMM.PageFrameNumber.Conversions", &TestEntry_PMM_PageFrameNumberConversions},
	{"Memory.PMM.ReferenceCount.RetainRelease", &TestEntry_PMM_ReferenceCount_RetainRelease},
	{"Memory.PagingHw.EnablePaging.TlbRefillSmoke", &TestEntry_PagingHw_EnablePaging_TlbRefillSmoke},
	{"Memory.PagingHw.UnmappedAccess.FaultsAndReportsBadV", &TestEntry_PagingHw_UnmappedAccess_FaultsAndReportsBadV},
	{"Memory.PagingHw.PagingFaultObserver.DispatchesAndCanHandle", &TestEntry_PagingHw_PagingFaultObserver_DispatchesAndCanHandle},
	{"Memory.PagingHw.PagingFaultObserver.MapsAndRetries", &TestEntry_PagingHw_PagingFaultObserver_MapsAndRetries},
	{"Memory.PagingHw.KernelPager.MapsAndRetries", &TestEntry_PagingHw_KernelPager_MapsAndRetries},
	{"Memory.PagingHw.VmmPager.MapsViaVmaAndVmObject", &TestEntry_PagingHw_VmmPager_MapsViaVmaAndVmObject},
	{"Memory.PagingHw.ReadOnlyStore.RaisesPME", &TestEntry_PagingHw_ReadOnlyStore_RaisesPme},
	{"Memory.PagingHw.NonExecutableFetch.RaisesPNX", &TestEntry_PagingHw_NonExecutableFetch_RaisesPnx},
	{"Memory.PagingHw.PostPaging.MapUnmap.Faults", &TestEntry_PagingHw_PostPaging_MapUnmap_Faults},
	{"Memory.PagingHw.HigherHalfStack.GuardPageFaults", &TestEntry_PagingHw_HigherHalfStack_GuardPageFaults},
	{"Memory.PagingHw.AddressSpaces.SwitchPgdlChangesTranslation", &TestEntry_PagingHw_AddressSpaces_SwitchPgdlChangesTranslation},
};

extern const std::size_t g_test_case_count = sizeof(g_test_cases) / sizeof(g_test_cases[0]);

} // namespace Rocinante::Testing
