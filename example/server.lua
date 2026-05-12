RegisterCommand("count", function(source)
    local count = exports.example:getPlayerCount()
    print(("[example:lua] Player count from C++: %d"):format(count))
end, false)

RegisterCommand("whoami", function(source)
    local name = exports.example:getPlayerName(tostring(source))
    if name then
        print(("[example:lua] Player %d is: %s"):format(source, name))
    else
        print(("[example:lua] Player %d not found"):format(source))
    end
end, false)

RegisterCommand("state", function(source)
    local count = GlobalState.playerCount
    print(("[example:lua] Global playerCount (from C++ statebag): %s"):format(tostring(count)))
    if source > 0 then
        local name = Player(source).state.name
        print(("[example:lua] Your name (from C++ statebag): %s"):format(tostring(name)))
    end
end, false)
