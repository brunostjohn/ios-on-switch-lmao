// TegraGM20BRegDump.h against the capture's register list (regs_gm20b.txt, the same file L4T's capture.sh reads):
// the table is that list's readable offsets in its order, under its id, and the dump's lines are the capture's
// "0xOFFSET 0xVALUE" form, one value per offset.
#include <cstdio>

#include "drivers/TegraGM20B/TegraGM20BRegDump.h"

#include <gtest/gtest.h>

#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace
{

struct List {
	std::string id;
	std::vector<uint32_t> read;
	std::set<uint32_t> skipped;
};

List Load()
{
	std::ifstream file(REGS_TXT);
	List list;
	for (std::string line; std::getline(file, line);) {
		std::smatch m;
		if (std::regex_search(line, m, std::regex(R"(^# GM20B register list v1 list=([0-9a-f]{16}))"))) {
			list.id = m[1].str();
			continue;
		}
		if (line.empty() || line[0] == '#')
			continue;
		const uint32_t offset = std::stoul(line.substr(0, line.find(' ')), nullptr, 16);
		if (line.find(" skip=") != std::string::npos)
			list.skipped.insert(offset);
		else
			list.read.push_back(offset);
	}
	return list;
}

std::vector<uint32_t> Values()
{
	std::vector<uint32_t> values;
	for (size_t i = 0; i < TGM_REGDUMP_COUNT; i++)
		values.push_back(tgm_regdump_offsets[i] ^ 0xa5a5a5a5u);
	return values;
}

TEST(Gm20bRegdump, TheTableIsTheCapturesListInItsOrder)
{
	const List list = Load();
	char id[17];
	snprintf(id, sizeof(id), "%016llx", static_cast<unsigned long long>(TGM_REGDUMP_LIST_ID));
	EXPECT_EQ(list.id, id);
	ASSERT_EQ(list.read.size(), TGM_REGDUMP_COUNT);
	for (size_t i = 0; i < TGM_REGDUMP_COUNT; i++) {
		EXPECT_EQ(list.read[i], tgm_regdump_offsets[i]) << i;
		EXPECT_EQ(list.skipped.count(tgm_regdump_offsets[i]), 0u) << i;
	}
}

TEST(Gm20bRegdump, EachLineIsOneRegisterInListOrder)
{
	std::vector<char> buf(TGM_REGDUMP_BYTES);
	const std::vector<uint32_t> values = Values();
	const tgm_regdump_how how = {2, 0x142028, 0x80002000, 0x80002000, 0x40000123, 0x40000123, 3456};
	const size_t len = tgm_regdump_format(buf.data(), buf.size(), values.data(), &how);
	ASSERT_GT(len, 0u);
	ASSERT_LT(len, buf.size());

	std::istringstream text(std::string(buf.data(), len));
	std::string line;
	ASSERT_TRUE(std::getline(text, line));
	char header[96];
	snprintf(header, sizeof(header), "# regdump v1 list=%016llx count=%u",
		 static_cast<unsigned long long>(TGM_REGDUMP_LIST_ID), static_cast<unsigned>(TGM_REGDUMP_COUNT));
	EXPECT_EQ(line, header);
	ASSERT_TRUE(std::getline(text, line));
	EXPECT_EQ(line, "# read badf=2 bus_fault=142028 engine0=80002000/80002000 fecs_ctx=40000123/40000123 "
			"read_us=3456");
	const std::regex form(R"(^0x([0-9a-f]{6}) 0x([0-9a-f]{8})$)");
	size_t i = 0;
	for (; std::getline(text, line); i++) {
		std::smatch m;
		ASSERT_TRUE(std::regex_match(line, m, form)) << line;
		EXPECT_EQ(std::stoul(m[1].str(), nullptr, 16), tgm_regdump_offsets[i]);
		EXPECT_EQ(std::stoul(m[2].str(), nullptr, 16), values[i]);
	}
	EXPECT_EQ(i, TGM_REGDUMP_COUNT);
}

TEST(Gm20bRegdump, AShortBufferGetsNothing)
{
	std::vector<char> buf(TGM_REGDUMP_BYTES - 1);
	const std::vector<uint32_t> values = Values();
	const tgm_regdump_how how = {};
	EXPECT_EQ(tgm_regdump_format(buf.data(), buf.size(), values.data(), &how), 0u);
}

TEST(Gm20bRegdump, PriErrorsAreCounted)
{
	std::vector<uint32_t> values(TGM_REGDUMP_COUNT, 0);
	values[3] = 0xbadf1100u;
	values[TGM_REGDUMP_COUNT - 1] = 0xbadf5040u;
	values[7] = 0x0badf000u;
	EXPECT_EQ(tgm_regdump_badf(values.data()), 2u);
}

} // namespace
