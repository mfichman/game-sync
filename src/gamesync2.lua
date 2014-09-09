-- Copyright (c) 2014 Matt Fichman
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


local m = {}

-- Create an object with arguments 'args' and default field values 'defaults'
function m.Object(args, defaults)
    local self = {}
    for k, v in pairs(defaults) do
        if self[k] == nil then
            self[k] = defaults[k]
        else
            self[k] = args[k]
        end 
    end
    return 
end

-- Creates an adapter for a table. The adapter reads input from the input
-- stream(s) and sends output to the output stream(s).
function m.Adapter(args)
    local self = m.Object(args, {
        sd=nil, -- socket
        table=nil,
    })
    assert(self.table)

    -- Send the given key and value. If the key/value can't be sent, then
    -- mark them as dirty and try again later.
    function self:send(id, k, v)
    end

    return self
end

-- Return the gamesync metatable for the table. This special table watches for
-- changes to the table, and then serializes the changes to any attached
-- outputs. 
function m.Metatable(args) 
    local self = m.Object(args, {
        table=nil,
        flags='r', -- is this table readable?
        adapter={}, -- set of adapters
        id=nil,
    })
    assert(self.table)
    assert(getmetatable(self.table)==nil)

    local data = {} -- private cached data

    -- Return the requested data from the cache
    function self.__index(t, k, v)
        return data[k]
    end

    -- Set the requested data for the cache, and then send the data on the
    -- output connections.
    function self.__newindex(t, k, v)
        data[k] = v
        for adapter in self.adapter do
            adapter:send(id, k, v)
        end
    end

    -- Clear all the keys from the table & copy to the backing data cache
    for k, v in pairs(self.table) do
        data[k] = v
        rawset(self.table, k, nil)
    end

    setmetatable(self.table, self)
    return self
end



