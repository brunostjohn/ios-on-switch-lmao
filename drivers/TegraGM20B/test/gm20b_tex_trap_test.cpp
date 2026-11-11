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

}  // namespace
