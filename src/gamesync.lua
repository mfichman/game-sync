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

local gsn = require('lib.gamesync')

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

-- Creates a new channel.  A channel keeps track of the list of output sockets
-- to write to and also keeps track of the input socket.
function gs.Channels.new()
    local self = {}
    setmetatable(self, gs.Channels)
    self.input = nil
    self.output = {}
    return self
end

-- The gs.Metatable table recursively overrides the __newindex metamethod. 
-- Whenever __newindex is called, the gs library attempts to serialize the 
-- value that was set.
gs.Metatable = {}
gs.Metatable.__index = gs.Metatable


-- Creates a new Metatable.  The Metatable for a gamesync table intercepts the
-- index and newindex events, writes to the underlying data table instead, 
-- and then serializes the write to the network if necessary.
function gs.Metatable.new(table, channels)
    local self = {}
    setmetatable(self, gs.Metatable)
    self.channels = channels
    self.dirty = {}
    self.data = {}
    self.id = gs.next_id
    gs.next_id = gs.next_id+1

    local mt = {}

    function mt:__newindex(key, value)
        return self:newindex(key, value) 
    end
    
    function mt:__index(key)
        return self:index(key)
    end

    setmetatable(table, self)
    return self
end

-- Serialize a value on all output channels if possible.  Add table to the
-- serialize list if there's no space in the output buffer.
function gs.Metatable:send(key, value)
    for _, sd in ipairs(self.channels.output) do
        gsn.send_begin(sd.sd)
        gsn.send_id(sd.sd, self.id)
        gsn.send_str(sd.sd, key) -- FIXME: Use an Atom table instead

        if type(value) == 'string' then
            gsn.send_typeid(sd.sd, string.byte('s'))
            gsn.send_str(sd.sd, value)  
            print('send', self.id, 's', key, value)
        elseif type(value) == 'number' then
            gsn.send_typeid(sd.sd, string.byte('n'))
            gsn.send_num(sd.sd, value)
            print('send', self.id, 'n', key, value)
        elseif type(value) == 'table' then
            gsn.send_typeid(sd.sd, string.byte('t'))
            gsn.send_id(sd.sd, value.id)
            print('send', self.id, 't', key, value)
        elseif type(value) == 'boolean' then
            gsn.send_typeid(sd.sd, string.byte('b'))
            gsn.send_bool(sd.sd, value)
            print('send', self.id, 'b', key, value)
        else
            error('invalid type')
        end
        if not gsn.send_end(sd.sd) then
            -- Not enough space. Add the table to the wait list 
            insert(sd.dirty, table)
        end
        gsn.send(sd.sd)
    end
end



-- Called when a user data table is changed.  Check if the write is idempotent.
-- If not, serialize the write.
function gs.Metatable:__newindex(key, value)
    if rawget(self.data, key) == value then
        return
    end
    rawset(self.data, key, value)
    if key:sub(1,1) == '_' then
        return
    end
    if type(value) == 'table' then
        gs.Metatable.new(value, channels)
    end
    self:send(key, value)
end

-- Return the value stored in the backing data table.
function gs.Metatable:__index(key)
    if key == 'id' then
        return self.id
    else
        return rawget(self.data, key)
    end
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

-- Receive a message from the socket.  If the whole message can't be read, try
-- again later when more bytes are available. 
function gs.Socket:recv()
    local sd = self.sd
    gsn.recv_begin(sd)

    local id = gsn.recv_id(sd)
    local key = gsn.recv_str(sd)
    local typeid = string.char(gsn.recv_typeid(sd)) 
    local value
    if typeid == 's' then
        value = gsn.recv_str(sd) 
    elseif typeid == 'n' then
        value = gsn.recv_num(sd)
    elseif typeid == 't' then
        local tableid = gsn.recv_id(sd)
        value = self.table[tableid]
        if not value then
            value = {}
            local channels = gs.Channels.new()
            gs.Metatable.new(value, channels, tableid)
            self.table[tableid] = value
        end
        insert(channels.output, sd)
    elseif typeid == 'b' then
        value = gsn.recv_boolean(sd)
    elseif typeid == '\0' then
        -- Socket is possibly borked
    else
        error('invalid typeid')
    end
    
    if not gsn.recv_end(sd) then
        return -- Couldn't read the whole message
    end
    local table = self.table[id]
    assert(table, 'unknown table id #'..id)
    table[key] = value
    print('recv', id, typeid, key, value)
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

    local channels = gs.Channels.new()
    local table = {}
    local mt = gs.Metatable.new(table, channels)
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
            insert(channels.output, sd)
            mt:send(path, table)
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

function gs.poll(wait) 
    local newsockets = {}
    gsn.poll(gs.socket, wait)
    for _, sd in pairs(gs.socket) do
        --if gsn.status(sd.sd) ~= 0 then
        --    sd:connect(sd.host, sd.port)
        if gsn.state(sd.sd) == 'listening' then
            if gsn.readable(sd.sd) then
                local ret = sd:accept()  
                insert(newsockets, ret)
            end
        else
            if gsn.writable(sd.sd) then
                gsn.send(sd.sd)
            end
            if gsn.readable(sd.sd) then
                gsn.recv(sd.sd)
                sd:recv()
            end
        end
    end
    for k, sd in ipairs(newsockets) do
        gs.socket[sd] = sd
    end
end

return gs
