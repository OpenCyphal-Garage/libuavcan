/*
 * Copyright (C) 2015 Pavel Kirienko <pavel.kirienko@gmail.com>
 */

#ifndef UAVCAN_PROTOCOL_FILE_SERVER_HPP_INCLUDED
#define UAVCAN_PROTOCOL_FILE_SERVER_HPP_INCLUDED

#include <uavcan/build_config.hpp>
#include <uavcan/debug.hpp>
#include <uavcan/node/service_server.hpp>
#include <uavcan/util/method_binder.hpp>
// UAVCAN types
#include <uavcan/protocol/file/GetInfo.hpp>
#include <uavcan/protocol/file/GetDirectoryEntryInfo.hpp>
#include <uavcan/protocol/file/Read.hpp>
#include <uavcan/protocol/file/Write.hpp>
#include <uavcan/protocol/file/Delete.hpp>

namespace uavcan
{
/**
 * The file server backend should implement this interface.
 */
class UAVCAN_EXPORT IFileServerBackend
{
public:
    typedef protocol::file::Path::FieldTypes::path Path;
    typedef protocol::file::EntryType EntryType;
    typedef protocol::file::Error Error;

    /**
     * Use this class to compute CRC64 for uavcan.protocol.file.GetInfo.
     */
    typedef DataTypeSignatureCRC FileCRC;

    /**
     * All read operations must return this number of bytes, unless end of file is reached.
     */
    enum { ReadSize = protocol::file::Read::Response::FieldTypes::data::MaxSize };

    /**
     * Shortcut for uavcan.protocol.file.Path.SEPARATOR.
     */
    static char getPathSeparator() { return static_cast<char>(protocol::file::Path::SEPARATOR); }

    /**
     * Backend for uavcan.protocol.file.GetInfo.
     * Implementation of this method is required.
     * On success the method must return zero.
     */
    virtual int16_t getInfo(const Path& path, uint64_t& out_crc64, uint32_t& out_size, EntryType& out_type) = 0;

    /**
     * Backend for uavcan.protocol.file.Read.
     * Implementation of this method is required.
     * @ref inout_size is set to @ref ReadSize; read operation is required to return exactly this amount, except
     * if the end of file is reached.
     * On success the method must return zero.
     */
    virtual int16_t read(const Path& path, const uint32_t offset, uint8_t* out_buffer, uint16_t& inout_size) = 0;

    // Methods below are optional.

    /**
     * Backend for uavcan.protocol.file.Write.
     * Implementation of this method is NOT required; by default it returns uavcan.protocol.file.Error.NOT_IMPLEMENTED.
     * On success the method must return zero.
     */
    virtual int16_t write(const Path& path, const uint32_t offset, const uint8_t* buffer, const uint16_t size)
    {
        (void)path;
        (void)offset;
        (void)buffer;
        (void)size;
        return Error::NOT_IMPLEMENTED;
    }

    /**
     * Backend for uavcan.protocol.file.Delete. ('delete' is a C++ keyword, so 'remove' is used instead)
     * Implementation of this method is NOT required; by default it returns uavcan.protocol.file.Error.NOT_IMPLEMENTED.
     * On success the method must return zero.
     */
    virtual int16_t remove(const Path& path)
    {
        (void)path;
        return Error::NOT_IMPLEMENTED;
    }

    /**
     * Backend for uavcan.protocol.file.GetDirectoryEntryInfo.
     * Implementation of this method is NOT required; by default it returns uavcan.protocol.file.Error.NOT_IMPLEMENTED.
     * On success the method must return zero.
     */
    virtual int16_t getDirectoryEntryInfo(const Path& directory_path, const uint32_t entry_index,
                                          EntryType& out_type, Path& out_entry_full_path)
    {
        (void)directory_path;
        (void)entry_index;
        (void)out_type;
        (void)out_entry_full_path;
        return Error::NOT_IMPLEMENTED;
    }

    virtual ~IFileServerBackend() { }
};

/**
 * Basic file server implements only the following services:
 *      uavcan.protocol.file.GetInfo
 *      uavcan.protocol.file.Read
 * Also see @ref IFileServerBackend.
 */
class BasicFileServer
{
    typedef MethodBinder<BasicFileServer*,
        void (BasicFileServer::*)(const protocol::file::GetInfo::Request&, protocol::file::GetInfo::Response&)>
            GetInfoCallback;

    typedef MethodBinder<BasicFileServer*,
        void (BasicFileServer::*)(const protocol::file::Read::Request&, protocol::file::Read::Response&)>
            ReadCallback;

    ServiceServer<protocol::file::GetInfo, GetInfoCallback> get_info_srv_;
    ServiceServer<protocol::file::Read, ReadCallback> read_srv_;

    void handleGetInfo(const protocol::file::GetInfo::Request& req, protocol::file::GetInfo::Response& resp)
    {
        resp.error.value = backend_.getInfo(req.path.path, resp.crc64, resp.size, resp.entry_type);
    }

    void handleRead(const protocol::file::Read::Request& req, protocol::file::Read::Response& resp)
    {
        uint16_t inout_size = resp.data.capacity();

        resp.data.resize(inout_size);

        resp.error.value = backend_.read(req.path.path, req.offset, resp.data.begin(), inout_size);

        if (inout_size > resp.data.capacity())
        {
            UAVCAN_ASSERT(0);
            resp.error.value = protocol::file::Error::UNKNOWN_ERROR;
        }
        else
        {
            resp.data.resize(inout_size);
        }
    }

protected:
    IFileServerBackend& backend_;       ///< Derived types can use it

public:
    BasicFileServer(INode& node, IFileServerBackend& backend)
        : get_info_srv_(node)
        , read_srv_(node)
        , backend_(backend)
    { }

    int start()
    {
        int res = get_info_srv_.start(GetInfoCallback(this, &BasicFileServer::handleGetInfo));
        if (res < 0)
        {
            return res;
        }

        res = read_srv_.start(ReadCallback(this, &BasicFileServer::handleRead));
        if (res < 0)
        {
            return res;
        }

        return 0;
    }
};

}

#endif // Include guard