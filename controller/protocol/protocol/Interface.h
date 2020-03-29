#pragma once
#include "../../../tools/Buffers.h"
#include "../../../tools/logging.h"
#include <memory>
#include <type_traits>

namespace tgvoip
{
struct VersionInfo;

template <typename T>
struct SingleChoice
{
    virtual ~SingleChoice() = default;
    static std::shared_ptr<T> choose(const BufferInputStream &in, const VersionInfo &ver)
    {
        return std::make_shared<T>();
    }

    void choose(BufferOutputStream &out, const VersionInfo &ver) const
    {
        return;
    }

    uint8_t getID() const
    {
        return 0;
    }

    static const uint8_t ID = 0;
};

template <typename T>
struct MultiChoice
{
public:
    virtual ~MultiChoice() = default;

    virtual void choose(BufferOutputStream &out, const VersionInfo &ver) const = 0;

    virtual uint8_t getID() const = 0;

    virtual size_t getConstructorSize(const VersionInfo &ver) const = 0;
};
/*
template <class T, class = void>
struct has_flags : std::false_type
{
};

template <class T>
struct has_flags<T, std::void_t<typename T::Flags>> : std::true_type
{
};

struct Empty
{
    enum Flags : uint8_t
    {
    };
};

template <typename T>
struct Constructor // : public T
{
    uint8_t getID() const
    {
        return T::ID;
    }
    using Flags = typename std::conditional<tgvoip::has_flags<T>::value, T, Empty>::type::Flags;
};
*/
struct Serializable
{
public:
    virtual ~Serializable() = default;

    virtual bool parse(const BufferInputStream &in, const VersionInfo &ver) = 0;
    virtual void serialize(BufferOutputStream &out, const VersionInfo &ver) const = 0;

    virtual std::string print() const = 0;
    virtual size_t getSize(const VersionInfo &ver) const = 0;
};

template <typename T>
struct Mask : public Serializable, SingleChoice<Mask<T>>
{
    virtual ~Mask() = default;

    bool parse(const BufferInputStream &in, const VersionInfo &ver) override
    {
        uint8_t mask;
        if (!in.TryRead(mask))
        {
            return false;
        }
        for (uint8_t i = 0; i < 8; i++)
        {
            if (!(mask & (1 << i)))
                continue;

            if (!in.TryRead(v.at(i), ver))
                return false;
        }
        return true;
    }
    void serialize(BufferOutputStream &out, const VersionInfo &ver) const override
    {
        uint8_t mask = 0;
        for (uint8_t i = 0; i < 8; i++)
        {
            if (v[i])
            {
                mask |= 1 << i;
            }
        }
        out.WriteByte(mask);
        for (const auto &data : v)
        {
            if (data)
                out.Write(data, ver);
        }
    }
    auto begin() noexcept
    {
        return v.begin();
    }
    auto end() noexcept
    {
        return v.end();
    }
    const auto begin() const noexcept
    {
        return v.begin();
    }
    const auto end() const noexcept
    {
        return v.end();
    }
    operator bool() const
    {
        for (const auto &data : v)
        {
            if (data)
                return true;
        }
        return false;
    }
    std::string print() const override
    {
        std::string res = "|";
        for (const auto &data : v)
        {
            res += (data ? data.print() : "");
            res += "|";
        }
        return res;
    }
    size_t getSize(const VersionInfo &ver) const override
    {
        size_t res = 1;
        for (const auto &data : v)
        {
            if (data)
                res += data.getSize(ver);
        }
        return res;
    }
    std::array<T, 8> v{};
};

template <typename T>
struct Array : public Serializable, SingleChoice<Array<T>>
{
    virtual ~Array() = default;

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
            auto data = T::choose(in, ver);
            if (!data || !data->parse(in, ver))
                return false;
            v.push_back(std::move(*data));
        }
        return true;
    }
    void serialize(BufferOutputStream &out, const VersionInfo &ver) const override
    {
        out.WriteByte(v.size());
        for (auto &data : v)
        {
            data.choose(out, ver);
            data.serialize(out, ver);
        }
    }
    auto begin() noexcept
    {
        return v.begin();
    }
    auto end() noexcept
    {
        return v.end();
    }
    const auto begin() const noexcept
    {
        return v.begin();
    }
    const auto end() const noexcept
    {
        return v.end();
    }
    operator bool() const
    {
        return !v.empty();
    }
    size_t getSize(const VersionInfo &ver) const override
    {
        size_t res = 1;
        for (const auto &data : v)
        {
            res += data.getSize(ver);
        }
        return res;
    }
    std::string print() const override
    {
        std::string res;
        for (const auto &val : v)
        {
            res += val.print();
            res += ", ";
        }
        return res;
    }
    std::vector<T> v;
};

template <class T>
struct Wrapped : public Serializable, SingleChoice<Wrapped<T>>
{
    virtual ~Wrapped() = default;

    Wrapped(std::shared_ptr<T> &&_d) : d(_d){};
    Wrapped(std::shared_ptr<T> &_d) : d(_d){};
    Wrapped() = default;

    bool parse(const BufferInputStream &in, const VersionInfo &ver) override
    {
        uint8_t len;
        if (!in.TryRead(len))
            return false;
        if (in.Remaining() < len)
            return false;
#ifdef LOG_PACKETS
        LOGW("Got buffer of length %hhu", len);
#endif
        auto buf = in.GetPartBuffer(len);
        d = T::choose(buf, ver);
        if (!d)
        {
            return false;
        }
        return d->parse(buf, ver);
    }
    void serialize(BufferOutputStream &out, const VersionInfo &ver) const override
    {
        out.Advance(1);
        size_t len = out.GetOffset();
        if (d)
        {
            d->choose(out, ver);
            d->serialize(out, ver);
        }
        len = out.GetOffset() - len;
        out.Rewind(len + 1);
        out.WriteByte(len);
        out.Advance(len);
    }
    operator bool() const
    {
        return d && *d;
    }

    uint8_t getID() const
    {
        return d->getID();
    }

    template <typename X>
    X &get()
    {
        return *dynamic_cast<X *>(d.get());
    }

    template <typename X>
    const X &get() const
    {
        return *dynamic_cast<X *>(d.get());
    }

    template <typename X, typename = typename std::enable_if<!std::is_same_v<X, bool>>::type>
    operator X &()
    {
        return *dynamic_cast<X *>(d.get());
    }

    template <typename X, typename = typename std::enable_if<!std::is_same_v<X, bool>>::type>
    operator const X &() const
    {
        return *dynamic_cast<X *>(d.get());
    }

    std::string print() const override
    {
        return d->print();
    }
    size_t getSize(const VersionInfo &ver) const override
    {
        return 1 + d->getSize(ver);
    }
    std::shared_ptr<T> d;
};

class OutputBytes;
struct Bytes : public Serializable,
               SingleChoice<OutputBytes>
{
    virtual ~Bytes() = default;
    Bytes() = default;

    bool parse(const BufferInputStream &in, const VersionInfo &ver) override
    {
        setData(std::make_unique<Buffer>(in.GetLength()));
        return in.TryRead(*getData());
    }
    void serialize(BufferOutputStream &out, const VersionInfo &ver) const override
    {
        out.WriteBytes(*getData());
    }
    operator bool()
    {
        return getData() && !getData()->IsEmpty();
    }
    std::string print() const override
    {
        return "buffer";
    }
    size_t getSize(const VersionInfo &ver) const override
    {
        return getData()->Length();
    }

    virtual Buffer *getData() = 0;
    virtual const Buffer *getData() const = 0;
    virtual void setData(std::unique_ptr<Buffer> &&) = 0;
};

struct OutputBytes : public Bytes
{
    virtual ~OutputBytes() = default;
    OutputBytes() = default;
    OutputBytes(Buffer &&_data) : data(std::make_unique<Buffer>(std::move(_data))){};

    virtual Buffer *getData() override
    {
        return data.get();
    }
    virtual const Buffer *getData() const override
    {
        return data.get();
    }

    virtual void setData(std::unique_ptr<Buffer> &&buf) override
    {
        data = std::move(buf);
    }

    std::unique_ptr<Buffer> data;
};
struct InputBytes : public Bytes
{
    virtual ~InputBytes() = default;
    InputBytes() = default;
    InputBytes(std::shared_ptr<Buffer> _data) : data(_data){};

    virtual Buffer *getData() override
    {
        return data.get();
    }
    virtual const Buffer *getData() const override
    {
        return data.get();
    }
    virtual void setData(std::unique_ptr<Buffer> &&buf) override
    {
        data = std::move(buf);
    }

    std::shared_ptr<Buffer> data;
};

struct UInt32 : public Serializable,
                SingleChoice<UInt32>
{
    virtual ~UInt32() = default;
    UInt32() = default;
    UInt32(uint32_t _data) : data(_data){};
    operator uint32_t() const
    {
        return data;
    }
    bool parse(const BufferInputStream &in, const VersionInfo &ver) override
    {
        return in.TryRead(data);
    }
    void serialize(BufferOutputStream &out, const VersionInfo &ver) const override
    {
        out.WriteUInt32(data);
    }
    std::string print() const override
    {
        return std::to_string(data);
    }

    size_t getSize(const VersionInfo &ver) const override
    {
        return 4;
    }
    uint32_t data;
};

/*
namespace Template
{
template <class T>
struct Constructor : public T, tgvoip::Constructor<T>
{
};
}; // namespace Template
*/
} // namespace tgvoip