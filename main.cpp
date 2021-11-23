// Maybe not all of these dependencies are needed.

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

uint32_t byteSwap32(uint32_t num)
{
    return ((num >> 24) & 0x000000FF) | ((num >> 8) & 0x0000FF00) | ((num << 8) & 0x00FF0000) | ((num << 24) & 0xFF000000);
}

struct Data {
    uint8_t* data = nullptr;
    size_t size;
};

struct Stream {
    uint8_t* data = nullptr;
    size_t offset;
};

struct Reader {
    Reader(const char* fileName)
    {
        this->f = fopen(fileName, "rb");
        if (!this->f)
            throw std::runtime_error("file opening error");
        fseek(this->f, 0, SEEK_END);
        this->fileSize = ftell(f);
        this->buffer.data = static_cast<uint8_t*>(malloc(4));
        this->buffer.size = 4;
    }

    void read(uint64_t offset, size_t size)
    {
        uint64_t availableSize = fileSize - offset;
        // There is no error trying to read more bytes than the file has just read the reminder.
        if (size > availableSize)
            size = static_cast<size_t>(availableSize);
        if (size > 0) {
            if (fseek(this->f, offset, SEEK_SET) != 0) {
                free(buffer.data);
                buffer.data = nullptr;
                return;
            }
            if (buffer.size < size) {
                free(buffer.data);
                buffer.data = static_cast<uint8_t*>(malloc(size));
                buffer.size = size;
            }
            size_t bytesRead = fread(buffer.data, 1, size, this->f);
            if (size != bytesRead) {
                if (ferror(this->f)) {
                    free(buffer.data);
                    buffer.data = nullptr;
                    return;
                }
                size = bytesRead;
            }
        }
        this->buffer.size = size;
    };

    ~Reader()
    {
        if (f)
            fclose(f);
        if (buffer.data)
            free(buffer.data);
    }

    FILE* f;
    size_t fileSize;
    Data buffer;
};

int main(int argc, char* argv[])
{
    if (argc < 2) {
        std::cerr << "set path" << std::endl;
        return 1;
    }

    Reader* reader;
    try {
        reader = new Reader(argv[1]);
    } catch (const std::exception& e) {
        std::cerr << e.what() << std::endl;
        return 1;
    }

    uint64_t parseGap = 256;
    uint64_t parseOffset = 0;

    // 3 blocks contain the information we need.
    // The meta block contains all these blocks.
    // We need to check the presence of .ftyp to determine BMFF satisfies.
    // .avif block indicates the existence of a meta block.

    bool ftypSeen = false;
    bool needsMeta = false;
    bool metaSeen = false;

    bool needsIprp = false;
    bool iprpSeen = false;

    bool needsIpco = false;
    bool ipcoSeen = false;

    bool needsIspe = false;
    bool ispeSeen = false;

    bool needsPixi = false;
    bool pixiSeen = false;

    bool needsHdlr = false;
    bool hdlrSeen = false;

    bool needsIloc = false;
    bool ilocSeen = false;

    bool needsReread = false;

    uint32_t binaryDataOffset;
    uint32_t metaSize;
    uint32_t handlerSize;

    size_t metaSizeOffset;
    size_t handlerSizeOffset;
    size_t ilocOffset;

    std::string comment;

    if ((argc == 3) && !strcmp("--commedit", argv[2])) {
        needsHdlr = true;
        needsIloc = true;
    }

    uint8_t offsetSizeAndLengthSize;
    uint8_t offsetSize;
    uint8_t lengthSize;
    uint8_t baseOffsetSizeAndIndexSize;
    uint8_t baseOffsetSize;

    bool parseFlag = false;

    for (;;) {
        reader->read(parseOffset, parseGap);

        if (reader->buffer.data == nullptr)
            return 1;

        if (reader->buffer.size == 0)
            break;

        Stream stream;
        stream.data = reader->buffer.data;
        stream.offset = 0;

        if (!(ftypSeen && (!needsMeta || metaSeen)))
            for (; stream.offset < reader->buffer.size; stream.offset += 4)
                if (!memcmp(stream.data + stream.offset, "ftyp", 4)) {
                    if (ftypSeen)
                        return 1;
                    ftypSeen = true;
                } else if (!memcmp(stream.data + stream.offset, "avif", 4)) {
                    needsMeta = true;
                } else if (!memcmp(stream.data + stream.offset, "meta", 4)) {
                    if (!needsMeta || metaSeen)
                        return 1;
                    memcpy(&metaSize, stream.data + stream.offset - 4, 4);
                    metaSizeOffset = parseOffset + stream.offset - 4;
                    metaSeen = true;
                    needsIprp = true;
                    break;
                }

        if (!(!needsHdlr || hdlrSeen))
            for (; stream.offset < reader->buffer.size; stream.offset += 1)
                if (!memcmp(stream.data + stream.offset, "hdlr", 4)) {
                    if (hdlrSeen)
                        return 1;
                    memcpy(&handlerSize, stream.data + stream.offset - 4, 4);
                    handlerSizeOffset = parseOffset + stream.offset - 4;

                    if (byteSwap32(handlerSize) > (parseGap - stream.offset)) {
                        parseGap = handlerSize + 20;
                        needsReread = true;
                        break;
                    }

                    std::cout << "comment: " << (char*)(stream.data + stream.offset + 28) << std::endl;
                    hdlrSeen = true;
                    break;
                }

        if (needsReread) {
            parseOffset += stream.offset; // Now, all text of hdlr box in buffer
            needsReread = false;
            continue;
        }

        if (!(!needsIloc || ilocSeen))
            for (; stream.offset < reader->buffer.size; stream.offset += 1)
                if (!memcmp(stream.data + stream.offset, "iloc", 4)) {
                    if (ilocSeen)
                        return 1;

                    memcpy(&offsetSizeAndLengthSize, stream.data + stream.offset + 8, 1);
                    offsetSize = (offsetSizeAndLengthSize >> 4) & 0xf;
                    lengthSize = (offsetSizeAndLengthSize >> 0) & 0xf;
                    memcpy(&baseOffsetSizeAndIndexSize, stream.data + stream.offset + 9, 1);
                    baseOffsetSize = (baseOffsetSizeAndIndexSize >> 4) & 0xf;

                    if ((offsetSize == 0 || offsetSize == 4) && lengthSize == 4 && baseOffsetSize == 4)
                        memcpy(&binaryDataOffset, stream.data + stream.offset + 16, 4);
                    else if (offsetSize == 4 && lengthSize == 4 && baseOffsetSize == 0)
                        memcpy(&binaryDataOffset, stream.data + stream.offset + 18, 4);

                    ilocOffset = parseOffset + stream.offset;
                    ilocSeen = true;
                    break;
                }

        if (!(!needsIprp || iprpSeen))
            for (; stream.offset < reader->buffer.size; stream.offset += 1)
                if (!memcmp(stream.data + stream.offset, "iprp", 4)) {
                    if (iprpSeen)
                        return 1;
                    iprpSeen = true;
                    needsIpco = true;
                    break;
                }

        if (!(!needsIpco || ipcoSeen))
            for (; stream.offset < reader->buffer.size; stream.offset += 1)
                if (!memcmp(stream.data + stream.offset, "ipco", 4)) {
                    if (ipcoSeen)
                        return 1;
                    ipcoSeen = true;
                    needsIspe = true;
                    needsPixi = true;
                    break;
                }

        if (!((!needsIspe || ispeSeen) && (!needsPixi || pixiSeen))) {
            for (; stream.offset < reader->buffer.size; stream.offset += 1) {
                if (!memcmp(stream.data + stream.offset, "ispe", 4)) {
                    if (ispeSeen)
                        return 1;
                    ispeSeen = true;

                    if (reader->buffer.size - stream.offset < 16) {
                        needsReread = true;
                        ispeSeen = false;
                        break;
                    }

                    std::cout << "width: " << (((uint32_t)(*(stream.data + stream.offset + 8)) << 24) | ((uint32_t)(*(stream.data + stream.offset + 9)) << 16) | ((uint32_t)(*(stream.data + stream.offset + 10)) << 8) | ((uint32_t)(*(stream.data + stream.offset + 11)) << 0)) << std::endl;
                    std::cout << "height: " << (((uint32_t)(*(stream.data + stream.offset + 12)) << 24) | ((uint32_t)(*(stream.data + stream.offset + 13)) << 16) | ((uint32_t)(*(stream.data + stream.offset + 14)) << 8) | ((uint32_t)(*(stream.data + stream.offset + 15)) << 0)) << std::endl;

                    continue;
                } else if (!memcmp(stream.data + stream.offset, "pixi", 4)) {
                    if (pixiSeen)
                        return 1;

                    if (reader->buffer.size - stream.offset < 16) {
                        needsReread = true;
                        break;
                    }
                    pixiSeen = true;

                    std::cout << "channels: " << ((uint8_t)(*(stream.data + stream.offset + 8)) << 0) << std::endl;
                    std::cout << "depth (bit) per channel: " << ((uint8_t)(*(stream.data + stream.offset + 9)) << 0) << ", " << ((uint8_t)(*(stream.data + stream.offset + 10)) << 0) << ", " << ((uint8_t)(*(stream.data + stream.offset + 11)) << 0) << std::endl;

                    continue;
                }
            }
        }
        parseOffset += stream.offset;

        if (needsReread) {
            needsReread = false;
            continue;
        }

        if ((needsHdlr || needsMeta) && (!hdlrSeen || !metaSeen))
            parseOffset -= 7;
        else if (needsIprp || needsIpco || needsIspe || needsPixi)
            parseOffset -= 3;

        if (ftypSeen && (!needsMeta || metaSeen) && (!needsIprp || iprpSeen) && (!needsIpco || ipcoSeen) && (!needsIspe || ispeSeen) && (!needsPixi || pixiSeen) && (!needsHdlr || hdlrSeen) && (!needsIloc || ilocSeen)) {
            parseFlag = true;
            break;
        }
    }

    if (!parseFlag)
        return 0;
    else if (needsHdlr) {
        std::cout << "type new comment: ";
        std::string newComment;
        std::getline(std::cin, newComment);

        int difference = newComment.size() - (byteSwap32(handlerSize) - 32 - 1);

        size_t afterHdlrOffset = handlerSizeOffset + 4 + (byteSwap32(handlerSize)) - 4;
        size_t afterHdlr = ilocOffset - afterHdlrOffset;
        metaSize = byteSwap32(byteSwap32(metaSize) + difference);
        handlerSize = byteSwap32(byteSwap32(handlerSize) + difference);
        binaryDataOffset = byteSwap32(byteSwap32(binaryDataOffset) + difference);
        std::string filename = "new_" + std::string(argv[1]);

        FILE* f = fopen(filename.c_str(), "wb");

        if (!f) {
            return 1;
        }

        reader->read(0, reader->fileSize);
        fwrite(reader->buffer.data, 1, metaSizeOffset, f);
        fwrite(&metaSize, 4, 1, f);
        fwrite(reader->buffer.data + metaSizeOffset + 4, 1, handlerSizeOffset - metaSizeOffset - 4, f);
        fwrite(&handlerSize, 4, 1, f);
        fwrite(reader->buffer.data + handlerSizeOffset + 4, 28, 1, f);
        fwrite(newComment.c_str(), 1, newComment.size() + 1, f);
        fwrite(reader->buffer.data + afterHdlrOffset, 1, afterHdlr, f);

        if ((offsetSize == 0 || offsetSize == 4) && lengthSize == 4 && baseOffsetSize == 4) {
            fwrite(reader->buffer.data + ilocOffset, 1, 16, f);
            fwrite(&binaryDataOffset, 4, 1, f);
            fwrite(reader->buffer.data + ilocOffset + 20, 1, reader->fileSize - (ilocOffset + 20), f);
        } else if (offsetSize == 4 && lengthSize == 4 && baseOffsetSize == 0) {
            fwrite(reader->buffer.data + ilocOffset, 1, 18, f);
            fwrite(&binaryDataOffset, 4, 1, f);
            fwrite(reader->buffer.data + ilocOffset + 22, 1, reader->fileSize - (ilocOffset + 22), f);
        }

        if (f) {
            fclose(f);
        }
        return 0;
    }
}