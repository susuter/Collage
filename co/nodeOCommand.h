
/* Copyright (c) 2012, Daniel Nachbaur <danielnachbaur@gmail.com>
 *
 * This library is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License version 2.1 as published
 * by the Free Software Foundation.
 *
 * This library is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License for more
 * details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#ifndef CO_NODEOCOMMAND_H
#define CO_NODEOCOMMAND_H

#include <co/dataOStream.h>   // base class


namespace co
{

namespace detail { class NodeOCommand; }

/**
 * A class for sending commands and data to local & external nodes.
 *
 * The data to this command is added via the interface provided by DataOStream.
 * The command is send or dispatched after it goes out of scoped, i.e. during
 * destruction.
 */
class NodeOCommand : public DataOStream
{
public:
    /**
     * Construct a command which is send & dispatched typically to a co::Node.
     *
     * @param connections list of connections where to send the command to.
     * @param cmd the command.
     * @param type the command type for dispatching.
     */
    CO_API NodeOCommand( const Connections& receivers, const uint32_t cmd,
                         const uint32_t type = COMMANDTYPE_CO_NODE );

    /**
     * Construct a command which is dispatched locally typically to a co::Node.
     *
     * @param dispatcher the dispatcher to dispatch this command.
     * @param localNode the local node that holds the command cache.
     * @param cmd the command.
     * @param type the command type for dispatching.
     */
    CO_API NodeOCommand( Dispatcher* const dispatcher, LocalNodePtr localNode,
                         const uint32_t cmd,
                         const uint32_t type = COMMANDTYPE_CO_NODE );

    /** @internal */
    NodeOCommand( NodeOCommand const& rhs );

    /** Send or dispatch this command during destruction. */
    CO_API virtual ~NodeOCommand();

    /**
     * Allow external send of data along with this command.
     *
     * @param additionalSize size in bytes of external data.
     */
    void setExternalSend( const uint64_t additionalSize );

    /** @return the static size of this command. */
    CO_API static size_t getSize();

protected:
    virtual void sendData( const void* buffer, const uint64_t size,
                           const bool last );

private:
    detail::NodeOCommand* const _impl;

    void _init();
};
}

#endif //CO_NODEOCOMMAND_H
