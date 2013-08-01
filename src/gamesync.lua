-- Copyright (c) 2013 Matt Fichman
-- 
-- Permission is hereby granted, free of charge, to any person obtaining a copy
-- of this software and associated documentation files (the "Software"), to
-- deal in the Software without restriction, including without limitation the
-- rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
-- sell copies of the Software, and to permit persons to whom the Software is
-- furnished to do so, subject to the following conditions:
-- 
-- The above copyright notice and this permission notice shall be included in
-- all copies or substantial portions of the Software.
-- 
-- THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, APEXPRESS OR
-- IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
-- FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
-- AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
-- LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
-- FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
-- IN THE SOFTWARE.

gsn = require('gamesyncnative')

local gs = {}
gs.socket = {} -- array of sockets
gs.table = { _id = 0 } -- list of root tables by path
gs.next_id = 1 -- next ID to use for a table

local insert = table.insert

-- For a given root table, records the input/output sockets for the table.  
-- Each root table can have at most 1 input socket, but any number of output
-- sockets.
gs.Channels = {}
gs.Channels.__index = gs.Channels

function gs.Channels.new()
    local self = {}
    setmetatable(self, gs.Channels)
    self.input = nil
    self.output = {}
    return self
end

-- Serialize a value on all output channels if possible.  Add table to the
-- serialize list if there's no space in the output buffer.
function gs.Channels:send(table, key, value)
    for _, sd in ipairs(self.output) do
        gsn.begin(sd.sd)
        gsn.send_id(sd.sd, table._id)
        gsn.send_str(sd.sd, key) -- FIXME: Use an Atom table instead
        if type(value) == 'string' then
            gsn.send_typeid(sd.sd, 's')
            gsn.send_str(sd.sd, value)  
        elseif type(value) == 'number' then
            gsn.send_typeid(sd.sd, 'n')
            gsn.send_num(sd.sd, value)
        elseif type(value) == 'table' then
            gsn.send_typeid(sd.sd, 't')
            gsn.send_id(sd.sd, value._id)
        elseif type(value) == 'boolean' then
            gsn.send_typeid(sd.sd, 'b')
            gsn.send_bool(sd.sd, value)
        else
            assert(false, 'invalid type')
        end
        if not gsn.commit(sd.sd) then
            -- Not enough space. Add the table to the wait list 
            table.insert(sd.sd.dirty, table)
        end
    end
end

-- Receive a message from the socket.  If the whole message can't be read, try
-- again later when more bytes are available. 
function gs.Channels:recv()
    local sd = self.input
    local id = gsn.recv_id(sd.sd)
    local key = gsn.recv_str(sd.sd)
    local typeid = gsn.recv_typeid(sd.sd) 
    local value
    if typeid == 's' then
        value = gsn.recv_str(sd.sd) 
    elseif typeid == 'n' then
        value = gsn.recv_num(sd.sd)
    elseif typeid == 't' then
        local tableid = gsn.recv_id(sd.sd)
        value = sd.table[tableid]
        if not value then
            value = { _id = tableid }
            sd.table[tableid] = value
        end
    elseif typeid == 'b' then
        value = gsn.recv_boolean(sd.sd)
    else
        assert(false, 'invalid type')
    end
    if gsn.consume() then
        return -- Couldn't read the whole message
    end
    local table = sd.table[id]
    assert(table, 'unknown table id #'..id)
    table[key] = value
end


-- The gs.Metatable table recursively overrides the __newindex metamethod. 
-- Whenever __newindex is called, the gs library attempts to serialize the 
-- value that was set.
gs.Metatable = {}

-- Attach a metatable to the nested table if necessary, and send the update 
-- if the table is connected.
function gs.Metatable:__newindex(key, value)
    rawset(self, key, value)
    if key:sub(1,1) == '_' then
        return
    end

    print(self, key, value)
    if type(value) == 'table' then
        setmetatable(value, gs.Metatable) 
        value._channels = self._channels
        if value._id == nil then
            value._id = gs.next_id
            gs.next_id = gs.next_id + 1
        end
    end
    self._channels:send(self, key, value)
end

-- The gs.Socket table keeps a connection up so that tables stay in-sync,
-- and reconnects if necessary.
gs.Socket = {}
gs.Socket.__index = gs.Socket

-- Create a new persistent connection with another process.
function gs.Socket.new()
    local self = {}
    setmetatable(self, gs.Socket)
    self.dirty = {} -- Tables that need be written to output
    self.table = {} -- Tables listed by opposite endpoint id
    self.table[0] = gs.table
    return self
end

-- Reconnect the socket to the endpoint.
function gs.Socket:connect(host, port)
    if self.sd then
        self:close()
    end
    self.sd = gsn.socket()
    gsn.connect(self.sd, host, port)
end

-- Listen on a port
function gs.Socket:listen(port)
    self.sd = gsn.socket()
    gsn.listen(self.sd, port)
end

-- Accept
function gs.Socket:accept()
    local ret = gs.Socket.new()
    ret.sd = gsn.accept(self.sd)
    return ret
end

-- Disconnect the socket from the endpoint.
function gs.Socket:close()
    gsn.close(self.sd)
    self.sd = nil
end

-- Parse a URI and return the hostname, port, and path
function gs.uri(uri) 
    local first, last = uri:find('^.+://')
    local scheme, host, port, path
    if first == nil then 
        scheme = 'local'
        pos = 0
    else
        scheme = uri:sub(first, last-3)
        pos = last + 1
    end

    first, last = uri:find('^.+:', pos)
    if first == nil then
        host = '' 
    else
        host = uri:sub(first, last-1)
        pos = last + 1
    end

    first, last = uri:find('^%d+', pos)
    if first == nil then 
        port = 0
    else
        port = uri:sub(first, last) 
        pos = last + 1
    end

    first, last = uri:find('^/.+$', pos) 
    if first == nil then return end
    path = uri:sub(first, last)

    return scheme, host, port, path
end


function gs.open_remote(host, port)
end

-- Open the table given by 'src' and 
function gs.open(src) 
    local scheme, host, port, path = gs.uri(src)
    if scheme == nil then
        return nil, 'error: bad uri'
    end
    local table = gs.table[path]
    if table then
        return nil, 'error: already open'
    end

    local table = {}
    table._channels = gs.Channels.new()
    table._id = gs.next_id
    gs.next_id = gs.next_id + 1
    setmetatable(table, gs.Metatable)
    gs.table[path] = table
    
    if scheme == 'local' then
        -- Do nothing
    elseif scheme == 'gs' then
        -- Connect
        local name = host..':'..port
        sd = gs.socket[name]
        if not sd then
            sd = gs.Socket.new()
            sd.host = host
            sd.port = port
            sd:connect(host, port)
            gs.socket[name] = sd
            insert(table._channels.output, sd)
            table._channels:send(gs.table, path, table)
        end
    else
        return nil, 'error: bad scheme'
    end
    return table
end

-- Listen on the given port
function gs.listen(port)
    local sd = gs.Socket.new()
    sd:listen(port) 
    gs.socket[port] = sd
end

function gs.close(src)

end

function gs.poll() 
    gsn.poll(gs.socket)
    for _, sd in pairs(gs.socket) do
        --if gsn.status(sd.sd) ~= 0 then
        --    sd:connect(sd.host, sd.port)
        if gsn.state(sd.sd) == 'listening' then
            if gsn.readable(sd.sd) then
                local ret = sd:accept()  
                gs.socket[ret] = ret 
                print(sd)
            end
        else
            if gsn.writable(sd.sd) then
                print('writable', sd)
            end
            if gsn.readable(sd.sd) then
                print('readable', sd)
            end
        end
    end
end

return gs
