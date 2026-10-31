/*
 * SPDX-FileCopyrightText: 2026 Euro-Office contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#include <cstdint>
#include <cstring>

#include "gtest/gtest.h"

#include "../src/ioapibuf.h"

// Unit tests for the in-memory minizip I/O callbacks in ioapibuf.c, which back
// every zip opened from a memory buffer (CunzFileWrapped::Open, CZipBuffer).
// They pin down the current read/seek behaviour on a read-only buffer; the
// write/grow path (zipOpenHelp) is not covered here.

// minizip-ng's compat layer (compat/ioapi.c, mz_stream_ioapi_seek) narrows the
// int64_t seek offset to int32_t before passing it as the callback's
// unsigned long parameter, so negative offsets arrive sign-extended.
static unsigned long CompatOffset(int64_t offset)
{
	return (unsigned long)(int32_t)offset;
}

// minizip-ng's compat layer maps a read-only open (unzOpen2) to this mode.
static const int kCompatReadMode = ZLIB_FILEFUNC_MODE_READ | ZLIB_FILEFUNC_MODE_EXISTING;

class CIoApiBufTest : public testing::Test
{
public:
	static constexpr unsigned long kSize = 16;

	unsigned char data[kSize];
	BUFFER_IO* io = NULL;
	zlib_filefunc_def ffunc;
	void* stream = NULL;

	void SetUp() override
	{
		for (unsigned long i = 0; i < kSize; ++i)
			data[i] = (unsigned char)i;

		// Same setup as CunzFileWrapped::Open and CZipBuffer::open: a heap
		// BUFFER_IO with only buffer/nSize set, wired up via
		// fill_buffer_filefunc, then opened by unzOpen2(NULL, &ffunc).
		io = new BUFFER_IO();
		io->buffer = data;
		io->nSize  = kSize;

		fill_buffer_filefunc(&ffunc, io);
		stream = ffunc.zopen_file(ffunc.opaque, NULL, kCompatReadMode);
	}

	void TearDown() override
	{
		if (stream)
		{
			EXPECT_EQ(ffunc.zclose_file(ffunc.opaque, stream), 0);
		}
		delete io; // RELEASEOBJECT in the callers
	}

	unsigned long Read(void* buf, unsigned long size)
	{
		return ffunc.zread_file(ffunc.opaque, stream, buf, size);
	}
	long Seek(unsigned long offset, int origin)
	{
		return ffunc.zseek_file(ffunc.opaque, stream, offset, origin);
	}
	long Tell()
	{
		return ffunc.ztell_file(ffunc.opaque, stream);
	}
	int Error()
	{
		return ffunc.zerror_file(ffunc.opaque, stream);
	}
};

TEST_F(CIoApiBufTest, open_read_returns_buffer)
{
	EXPECT_EQ(stream, (void*)io);
	EXPECT_EQ(io->buffer, &data[0]);
	EXPECT_EQ(io->nSize, kSize);
	EXPECT_EQ(io->nLimit, kSize);
	EXPECT_EQ(Tell(), 0);
	EXPECT_EQ(Error(), 0);
}

TEST_F(CIoApiBufTest, open_without_opaque_returns_null)
{
	EXPECT_EQ(ffunc.zopen_file(NULL, NULL, kCompatReadMode), nullptr);
}

TEST_F(CIoApiBufTest, error_without_stream_is_negative)
{
	EXPECT_EQ(ffunc.zerror_file(ffunc.opaque, NULL), -1);
}

TEST_F(CIoApiBufTest, read_within_bounds)
{
	unsigned char buf[4] = {};
	EXPECT_EQ(Read(buf, 4), 4u);
	EXPECT_EQ(memcmp(buf, data, 4), 0);
	EXPECT_EQ(Tell(), 4);
}

TEST_F(CIoApiBufTest, sequential_reads_advance)
{
	unsigned char buf[3] = {};
	ASSERT_EQ(Read(buf, 3), 3u);
	EXPECT_EQ(Read(buf, 3), 3u);
	EXPECT_EQ(memcmp(buf, data + 3, 3), 0);
	EXPECT_EQ(Tell(), 6);
}

TEST_F(CIoApiBufTest, read_whole_buffer)
{
	unsigned char buf[kSize] = {};
	EXPECT_EQ(Read(buf, kSize), kSize);
	EXPECT_EQ(memcmp(buf, data, kSize), 0);
	EXPECT_EQ(Tell(), (long)kSize);
}

TEST_F(CIoApiBufTest, read_zero_bytes)
{
	unsigned char buf[1] = {0xff};
	ASSERT_EQ(Seek(5, ZLIB_FILEFUNC_SEEK_SET), 0);
	EXPECT_EQ(Read(buf, 0), 0u);
	EXPECT_EQ(buf[0], 0xff);
	EXPECT_EQ(Tell(), 5);
}

TEST_F(CIoApiBufTest, read_across_end_is_short)
{
	unsigned char buf[8] = {};
	ASSERT_EQ(Seek(12, ZLIB_FILEFUNC_SEEK_SET), 0);
	EXPECT_EQ(Read(buf, 8), 4u);
	EXPECT_EQ(memcmp(buf, data + 12, 4), 0);
	EXPECT_EQ(Tell(), (long)kSize);
	// A short read is not reported as an error.
	EXPECT_EQ(Error(), 0);
}

TEST_F(CIoApiBufTest, read_at_end_returns_zero)
{
	unsigned char buf[4] = {};
	ASSERT_EQ(Seek(0, ZLIB_FILEFUNC_SEEK_END), 0);
	EXPECT_EQ(Read(buf, 4), 0u);
	EXPECT_EQ(Tell(), (long)kSize);
	EXPECT_EQ(Error(), 0);
}

TEST_F(CIoApiBufTest, seek_origins)
{
	EXPECT_EQ(Seek(5, ZLIB_FILEFUNC_SEEK_SET), 0);
	EXPECT_EQ(Tell(), 5);
	EXPECT_EQ(Seek(3, ZLIB_FILEFUNC_SEEK_CUR), 0);
	EXPECT_EQ(Tell(), 8);
	EXPECT_EQ(Seek(0, ZLIB_FILEFUNC_SEEK_END), 0);
	EXPECT_EQ(Tell(), (long)kSize);
	EXPECT_EQ(Error(), 0);
}

TEST_F(CIoApiBufTest, seek_set_to_end_succeeds)
{
	EXPECT_EQ(Seek(kSize, ZLIB_FILEFUNC_SEEK_SET), 0);
	EXPECT_EQ(Tell(), (long)kSize);
	EXPECT_EQ(Error(), 0);
}

TEST_F(CIoApiBufTest, seek_invalid_origin_fails)
{
	ASSERT_EQ(Seek(5, ZLIB_FILEFUNC_SEEK_SET), 0);
	EXPECT_EQ(Seek(0, 3), -1);
	EXPECT_EQ(Tell(), 5);
	// An invalid origin returns before the error flag is touched.
	EXPECT_EQ(Error(), 0);
}

TEST_F(CIoApiBufTest, seek_past_end_fails)
{
	ASSERT_EQ(Seek(5, ZLIB_FILEFUNC_SEEK_SET), 0);
	EXPECT_EQ(Seek(kSize + 1, ZLIB_FILEFUNC_SEEK_SET), -1);
	EXPECT_EQ(Tell(), 5);
	EXPECT_EQ(Error(), 1);
}

TEST_F(CIoApiBufTest, seek_cur_past_end_fails)
{
	ASSERT_EQ(Seek(10, ZLIB_FILEFUNC_SEEK_SET), 0);
	EXPECT_EQ(Seek(7, ZLIB_FILEFUNC_SEEK_CUR), -1);
	EXPECT_EQ(Tell(), 10);
	EXPECT_EQ(Error(), 1);
}

TEST_F(CIoApiBufTest, seek_end_past_end_fails)
{
	EXPECT_EQ(Seek(1, ZLIB_FILEFUNC_SEEK_END), -1);
	EXPECT_EQ(Tell(), 0);
	EXPECT_EQ(Error(), 1);
}

TEST_F(CIoApiBufTest, error_flag_is_sticky)
{
	ASSERT_EQ(Seek(kSize + 1, ZLIB_FILEFUNC_SEEK_SET), -1);
	ASSERT_EQ(Error(), 1);
	// Neither a successful seek nor a successful read clears it.
	unsigned char buf[2] = {};
	EXPECT_EQ(Seek(2, ZLIB_FILEFUNC_SEEK_SET), 0);
	EXPECT_EQ(Read(buf, 2), 2u);
	EXPECT_EQ(Error(), 1);
}

TEST_F(CIoApiBufTest, negative_seek_cur_moves_back)
{
	ASSERT_EQ(Seek(10, ZLIB_FILEFUNC_SEEK_SET), 0);
	EXPECT_EQ(Seek(CompatOffset(-4), ZLIB_FILEFUNC_SEEK_CUR), 0);
	EXPECT_EQ(Tell(), 6);
	EXPECT_EQ(Error(), 0);
}

TEST_F(CIoApiBufTest, negative_seek_cur_before_start_fails)
{
	ASSERT_EQ(Seek(3, ZLIB_FILEFUNC_SEEK_SET), 0);
	EXPECT_EQ(Seek(CompatOffset(-10), ZLIB_FILEFUNC_SEEK_CUR), -1);
	EXPECT_EQ(Tell(), 3);
	EXPECT_EQ(Error(), 1);
}

TEST_F(CIoApiBufTest, negative_seek_end_moves_back)
{
	unsigned char buf[2] = {};
	EXPECT_EQ(Seek(CompatOffset(-6), ZLIB_FILEFUNC_SEEK_END), 0);
	EXPECT_EQ(Tell(), 10);
	EXPECT_EQ(Read(buf, 2), 2u);
	EXPECT_EQ(memcmp(buf, data + 10, 2), 0);
}

TEST_F(CIoApiBufTest, negative_seek_set_fails)
{
	EXPECT_EQ(Seek(CompatOffset(-1), ZLIB_FILEFUNC_SEEK_SET), -1);
	EXPECT_EQ(Tell(), 0);
	EXPECT_EQ(Error(), 1);
}
