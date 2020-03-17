#pragma once
#include "../../../tools/Buffers.h"
#include <memory>

namespace tgvoip
{
struct VersionInfo
{
    int peerVersion = 0;
    int32_t connectionMaxLayer = 0; 
};

template <typename T>
struct SingleChoice
{
    static std::shared_ptr<T> choose(const BufferInputStream &in, VersionInfo &ver)
    {
        return std::make_shared<T>();
    }
};

struct Serializable
{
public:
    virtual bool parse(const BufferInputStream &in, VersionInfo &ver) = 0;
    virtual bool serialize(BufferOutputStream &out, VersionInfo &ver) = 0;
};

template <typename T>
struct Array : public Serializable, SingleChoice<Array>
{
public:
    bool parse(const BufferInputStream &in, VersionInfo &ver)
    {
        uint8_t extraCount = in.ReadByte();
        data.reserve(extraCount);
        for (auto i = 0; i < extraCount; i++)
        {
            BufferInputStream inExtra = in.GetPartBuffer(in.ReadByte());

            auto ptr = T::choose(inExtra, ver);

            if (!ptr->parse(inExtra, ver))
            {
                return false;
            }
            data.push_back(std::move(ptr));
        }
        return true;
    }
    bool serialize(BufferOutputStream &out, VersionInfo &ver)
    {
        out.WriteByte(data.size());
        for (auto &ptr : data)
        {
            if (!ptr->serialize(out, ver)) {
                return false;
            }
        }
        return true;
    }

    std::vector<std::shared_ptr<T>> data;
};

} // namespace tgvoip