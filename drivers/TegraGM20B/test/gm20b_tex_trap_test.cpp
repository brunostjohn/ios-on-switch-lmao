// TegraGM20BTexTrap.h: which GR traps are TEX exceptions alone, the TEX ESR's line and which repeats are logged; and that nouveau's
// interrupt handler takes those as nvgpu does (reported, the ESR written back, no TRAP error and so no kill), every other
// trap as before.
#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include "drivers/TegraGM20B/TegraGM20BTexTrap.h"

namespace
{

const uint8_t kTpcs[] = {2};

tgm_gr_trap_snapshot TexOn(unsigned int tpcs)
{
	tgm_gr_trap_snapshot s = {};
	s.trap = TGM_TRAP_GPC;
	s.gpcs = 1;
	s.gpc[0] = tpcs << 16;
	for (unsigned int tpc = 0; tpc < 2; ++tpc)
		if (tpcs & (1u << tpc))
			s.tpc[0][tpc] = TGM_TPC_EXCEPTION_TEX;
	return s;
}

TEST(Gm20bTexTrap, TexAloneOnEitherOrBothTpcs)
{
	for (unsigned int tpcs : {1u, 2u, 3u}) {
		const tgm_gr_trap_snapshot s = TexOn(tpcs);
		EXPECT_TRUE(tgm_gr_trap_tex_only(&s, 1, kTpcs)) << tpcs;
	}
}

TEST(Gm20bTexTrap, AnyOtherCauseKeepsTheTrap)
{
	tgm_gr_trap_snapshot s = TexOn(1);
	s.tpc[0][0] = 0x2;
	EXPECT_FALSE(tgm_gr_trap_tex_only(&s, 1, kTpcs)) << "SM";
	s.tpc[0][0] = 0x3;
	EXPECT_FALSE(tgm_gr_trap_tex_only(&s, 1, kTpcs)) << "TEX and SM";
	s = TexOn(3);
	s.tpc[0][1] = 0x4;
	EXPECT_FALSE(tgm_gr_trap_tex_only(&s, 1, kTpcs)) << "TEX on one TPC, POLY on the other";
	s = TexOn(1);
	s.trap |= 0x1;
	EXPECT_FALSE(tgm_gr_trap_tex_only(&s, 1, kTpcs)) << "with a DISPATCH trap";
	s = TexOn(1);
	s.gpc[0] |= 0x2;
	EXPECT_FALSE(tgm_gr_trap_tex_only(&s, 1, kTpcs)) << "with a ZCULL exception";
	s = TexOn(1);
	s.gpcs = 3;
	EXPECT_FALSE(tgm_gr_trap_tex_only(&s, 1, kTpcs)) << "a GPC that does not exist";
	s = TexOn(4);
	EXPECT_FALSE(tgm_gr_trap_tex_only(&s, 1, kTpcs)) << "a TPC that does not exist";
	s = TexOn(1);
	s.gpc[0] = 0;
	EXPECT_FALSE(tgm_gr_trap_tex_only(&s, 1, kTpcs)) << "no TPC named";
	tgm_gr_trap_snapshot none = {};
	EXPECT_FALSE(tgm_gr_trap_tex_only(&none, 1, kTpcs));
}

std::string Describe(uint32_t esr)
{
	char text[64];
	tgm_tex_esr_describe(esr, text, sizeof(text));
	return text;
}

TEST(Gm20bTexTrap, EsrNamesOnlyNvidiasFields)
{
	EXPECT_EQ(Describe(0x80000001u), "EN INTR, other bits 0");
	EXPECT_EQ(Describe(0xc0000005u), "EN RESET_ACTIVE INTR, other bits 0x4");
	EXPECT_EQ(Describe(0x00000100u), "-, other bits 0x100");
}

TEST(Gm20bTexTrap, EachChannelLogsItsFirstThenEveryDecade)
{
	tgm_tex_reports r = {};
	uint32_t count = 0;
	unsigned int logged = 0;
	for (uint32_t n = 1; n <= 1000; ++n) {
		const bool due = tgm_tex_report_due(&r, 5, 0x80000001u, &count);
		EXPECT_EQ(count, n);
		logged += due ? 1u : 0u;
		EXPECT_EQ(due, n == 1 || n == 10 || n == 100 || n == 1000) << n;
	}
	EXPECT_EQ(logged, 4u);
	EXPECT_TRUE(tgm_tex_report_due(&r, 5, 0x80000003u, &count)) << "a new ESR starts again";
	EXPECT_EQ(count, 1u);
}

TEST(Gm20bTexTrap, AlternatingChannelsKeepTheirOwnCounts)
{
	tgm_tex_reports r = {};
	uint32_t count = 0;
	unsigned int logged = 0;
	for (int n = 0; n < 200; ++n)
		logged += tgm_tex_report_due(&r, n % 2, 0x80000001u, &count) ? 1u : 0u;
	EXPECT_EQ(count, 100u);
	EXPECT_EQ(logged, 6u) << "1, 10 and 100 for each";
	for (int chid = 2; chid < 2 + TGM_TEX_CHANNELS; ++chid)
		EXPECT_TRUE(tgm_tex_report_due(&r, chid, 0x80000001u, &count));
	EXPECT_TRUE(tgm_tex_report_due(&r, 0, 0x80000001u, &count)) << "channel 0's slot was taken";
	EXPECT_EQ(count, 1u);
}

std::string Text(const char *path)
{
	std::ifstream file(path, std::ios::binary);
	std::stringstream text;
	text << file.rdbuf();
	return text.str();
}

std::string Body(const std::string &source, const std::string &head)
{
	const size_t at = source.find(head);
	if (at == std::string::npos)
		return "";
	return source.substr(at, source.find("\n}\n", at) - at);
}

TEST(Gm20bTexTrap, NouveauReportsTexAloneWithoutATrapError)
{
	const std::string gr = Text(GF100_C);
	const std::string intr = Body(gr, "gf100_gr_intr(struct nvkm_inth *inth)\n{");
	const size_t tex = intr.find("if (!gf100_gr_trap_tex(gr, chid, name)) {");
	const size_t trap = intr.find("nvkm_error(subdev, \"TRAP ch %d");
	const size_t others = intr.find("gf100_gr_trap_intr(gr);");
	ASSERT_NE(tex, std::string::npos);
	EXPECT_LT(tex, trap);
	EXPECT_LT(trap, others) << "every other trap is reported and handled as before";
	const std::string handler =
		Body(gr, "gf100_gr_trap_tex(struct gf100_gr *gr, int chid, const char *name)\n{");
	EXPECT_NE(handler.find("dev_warn(device->dev,\n\t\t\t\t\t \"gr: TEX exception %u of ch %d"),
		  std::string::npos)
		<< "not nvkm_warn: the device's \"error\" debug level drops warnings";
	EXPECT_EQ(handler.find("nvkm_error("), std::string::npos)
		<< "an nvkm error in the interrupt pass is a kill or a panic";
	EXPECT_NE(handler.find("nvkm_wr32(device, TPC_UNIT(gpc, tpc, 0x0224), esr);"),
		  std::string::npos)
		<< "nvgpu's clear: the ESR written back";
	EXPECT_LT(handler.find("if (!tgm_gr_trap_tex_only(&s, gr->gpc_nr, gr->tpc_nr))\n\t\treturn "
			       "false;"),
		  handler.find("nvkm_wr32("))
		<< "nothing is written for a trap that is not TEX alone";
	EXPECT_NE(Text(GK20A_C).find("nvkm_wr32(device, 0x419d0c, 0x3);"), std::string::npos);
}

// T-v2 kill_one and iterated_blend: each channel error gf100_gr_intr reports is handed to the kext before its
// acknowledgement lets GR finish the faulting job, so the kill record names that job, not the ones after it.
TEST(Gm20bGrFault, EveryChannelErrorIsChargedBeforeGrGoesOn)
{
	const std::string intr = Body(Text(GF100_C), "gf100_gr_intr(struct nvkm_inth *inth)\n{");
	for (const char *ack : {"nvkm_wr32(device, 0x400100, 0x00000010);",
				"nvkm_wr32(device, 0x400100, 0x00000020);",
				"nvkm_wr32(device, 0x400100, 0x00100000);",
				"nvkm_wr32(device, 0x400100, 0x00200000);"}) {
		const size_t at = intr.find(ack);
		ASSERT_NE(at, std::string::npos) << ack;
		const size_t fault = intr.rfind("tgm_gr_fault(chid);", at);
		ASSERT_NE(fault, std::string::npos) << ack;
		EXPECT_EQ(intr.find("nvkm_wr32(device, 0x400100", fault), at)
			<< ack << ": charged before its own ack";
	}
	const size_t sw = intr.find("if (!gf100_gr_mthd_sw(device, class, mthd, data)) {");
	EXPECT_LT(sw, intr.find("tgm_gr_fault(chid);")) << "a software method is no error";
	const size_t tex = intr.find("if (!gf100_gr_trap_tex(gr, chid, name)) {");
	EXPECT_LT(tex, intr.rfind("tgm_gr_fault(chid);")) << "a TEX-only trap is no error";
	EXPECT_LT(intr.rfind("tgm_gr_fault(chid);"),
		  intr.find("nvkm_wr32(device, 0x400500, 0x00010001);"));
	const std::string fault = Body(Text(GPU_FAULT_C), "void tgm_gr_fault(int chid)\n{");
	EXPECT_NE(fault.find("chan->fault_seqno = tgm_chan_completed(chan) + 1;"),
		  std::string::npos);
	EXPECT_NE(fault.find("tgm_fence_kill_record(chan->fence_cpu, chan->fault_seqno"),
		  std::string::npos)
		<< "recorded before the job's fence can pass it";
	const std::string killed =
		Body(Text(GPU_HANG_C), "int tgm_chan_killed(struct nvif_event *event");
	EXPECT_NE(killed.find("first = tgm_kill_first(chan->faulted, chan->fault_seqno"),
		  std::string::npos);
	EXPECT_NE(killed.find("tgm_fence_kill_record(chan->fence_cpu, first, chan->submitted"),
		  std::string::npos);
	const std::string free =
		Body(Text(GPU_CHAN_C), "static void chan_free(struct tgm_chan *chan)\n{");
	EXPECT_LT(free.find("tgm_fault_unpublish(chan);"),
		  free.find("nvif_object_dtor(&chan->user);"))
		<< "out of the table before nvkm drops the channel";
}

// T-v2 19:26:17-20: the PBDMA kept killed channel 20's method across GR's reset and GR loaded that context for 3 s
TEST(Gm20bGrFault, AKilledChannelsMethodIsDroppedAtEngReset)
{
	const std::string intr =
		Body(Text(RECOVER_C),
		     "static bool runq_intr(struct nvkm_runq *runq, struct nvkm_runl *runl)\n{");
	const size_t nop =
		intr.find("nvkm_wr32(device, PBDMA_METHOD0(runq->id), PBDMA_METHOD_NOP);");
	ASSERT_NE(nop, std::string::npos);
	EXPECT_LT(intr.find("const bool dead = !chan_alive(runq, chid);"), nop);
	EXPECT_LT(nop,
		  intr.find("nvkm_wr32(device, PBDMA_INTR_0(runq->id), TGM_PBDMA_ENG_RESET);"));
	const std::string hang = Body(Text(GPU_HANG_C), "enum tgm_hang tgm_hang_recover(");
	EXPECT_LT(hang.find("if (verdict == TGM_HANG_RESIDUE) {"),
		  hang.find("if (b < 0 || chans[b]->isolating)"));
	EXPECT_NE(hang.find("tgm_recover_residue(tgm_gpu.device, suspects[b])"), std::string::npos);
}

}  // namespace
