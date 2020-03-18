#pragma once
#include "../../../tools/Buffers.h"
#include <memory>

namespace tgvoip
{

struct VersionInfo
{
    VersionInfo(int _peerVersion, int32_t _connectionMaxLayer) : peerVersion(_peerVersion), connectionMaxLayer(_connectionMaxLayer){};
    const int peerVersion = 0;
    const int32_t connectionMaxLayer = 0;

    inline bool isNew() const
    {
        return peerVersion >= PROTOCOL_RELIABLE || connectionMaxLayer >= 110;
    }
};

template <typename T>
struct SingleChoice
{
    static std::shared_ptr<T> choose(const BufferInputStream &in, const VersionInfo &ver)
    {
        return std::make_shared<T>();
    }
};

struct Serializable
{
public:
    virtual bool parse(const BufferInputStream &in, const VersionInfo &ver) = 0;
    virtual void serialize(BufferOutputStream &out, const VersionInfo &ver) const = 0;
};

template <typename T>
struct Array : public Serializable, SingleChoice<Array<T>>
{
    bool parse(const BufferInputStream &in, const VersionInfo &ver) override
    {
        uint8_t extraCount;
        if (!in.TryRead(extraCount))
        {
            return false;
        }
        v.reserve(extraCount);
        for (auto i = 0; i < extraCount; i++)
        {
            auto ptr = T::choose(in, ver);
            if (!ptr || !ptr->parse(in, ver))
            {
                return false;
            }
            v.push_back(std::move(ptr));
        }
        return true;
    }
    void serialize(BufferOutputStream &out, const VersionInfo &ver) const override
    {
        out.WriteByte(v.size());
        for (auto &ptr : v)
        {
            ptr->serialize(out, ver);
        }
    }

    std::vector<std::shared_ptr<T>> v;
};

template <class T>
struct Wrapped : public Serializable, SingleChoice<Wrapped>
{
    bool parse(const BufferInputStream &in, const VersionInfo &ver) override
    {
        uint8_t len;
        if (!in.TryRead(len))
            return false;
        return d.parse(in.GetPartBuffer(len));
    }
    void serialize(BufferOutputStream &out, const VersionInfo &ver) const override
    {
        out.Advance(1);
        size_t len = out.GetOffset();
        d.serialize(out, ver);
        len = out.GetOffset() - len;
        out.Rewind(len + 1);
        out.WriteByte(len);
        out.Advance(len);
    }
    T d;
};

struct Bytes : public Serializable,
               SingleChoice<Bytes>
{
    bool parse(const BufferInputStream &in, const VersionInfo &ver)
    {
        data = Buffer(in.GetLength());
        return in.TryRead(data);
    }
    void serialize(BufferOutputStream &out, const VersionInfo &ver) const override
    {
        out.WriteBytes(data);
    }
    Buffer data;
};
struct UInt32 : public Serializable, SingleChoice<Bytes>
{
    bool parse(const BufferInputStream &in, const VersionInfo &ver)
    {
        return in.TryRead(data);
    }
    void serialize(BufferOutputStream &out, const VersionInfo &ver) const override
    {
        out.WriteUInt32(data);
    }
    uint32_t data;
};
} // namespace tgvoip