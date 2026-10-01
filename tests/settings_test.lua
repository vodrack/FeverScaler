-- Run from the repo root with Lua 5.4. Exercise the real settings script's group and bridge callbacks.
local disk, writes, userdata = nil, 0, {}
local function copy(t)
    local r = {}
    for k, v in pairs(t) do r[k] = v end
    return r
end
app = {
    getAllUserdata = function() return userdata end,
    loadUserdata = function() return copy(disk) end,
    saveUserdata = function(_, _, value) disk = copy(value); writes = writes + 1 end,
}
local tick = 0
os.clock = function() tick = tick + 1; return tick end
_ = function(text) return text end
-- Key names: the modifiers, then the scancode ("Ctrl+Shift+65").
local MODIFIER_NAMES = { [224] = "Ctrl", [225] = "Shift", [226] = "Alt" }
api = {
    type = {
        KeyComboDef = { new = function()
            return {
                mods = {},
                setKeyScancode = function(self, scancode) self.key = scancode end,
                addModifierKey = function(self, scancode) table.insert(self.mods, scancode) end,
            }
        end },
        KeyCombo = { new = function(def)
            return { toString = function()
                table.sort(def.mods)
                local parts = {}
                for _, m in ipairs(def.mods) do parts[#parts + 1] = MODIFIER_NAMES[m] end
                parts[#parts + 1] = tostring(def.key)
                return table.concat(parts, "+")
            end }
        end },
    },
    gui = {
        key = { State = { Pressed = "pressed", Up = "up" } },
        mouse = { Event = { Type = { PressedOutside = "outside" } } },
    },
}
-- Just enough of the game's UI modules. Recipes keep their hook state between calls (one instance
-- per recipe name). A settings page instance is a recipe whose node records the instance, its
-- parameters and a Label row made with the builtin the instance got when it loaded; instances
-- after the first can be made to fail.
local SETTINGS_PAGE = "::/gui/menu/settings_page.tl"
local hooks, hook = {}, 0
local current
local function cell(value)
    hook = hook + 1
    current[hook] = current[hook] or { value = value }
    return current[hook]
end
local ui = {
    RegisterRecipe = function(name, fn)
        return function(params)
            hooks[name] = hooks[name] or {}
            current, hook = hooks[name], 0
            local result = fn(params)
            assert(result.nodeType == "layout", name .. ": recipe child must be a layout")
            return result
        end
    end,
    useState = function(value)
        local c = cell(value)
        return { old = function() return c.value end, set = function(_, v) c.value = v end }
    end,
    useRef = function(value)
        local c = cell(value)
        return { get = function() return c.value end, set = function(_, v) c.value = v end }
    end,
    onMouseEvent = function() end,
    CallOriginalRecipe = function(recipe, params) return { original = recipe, params = params } end,
    BoxLayout = function(t) t.nodeType = "layout"; return t end,
    Button = function(t) return t end,
    TextView = function(t) return t end,
    type = { Orientation = { Horizontal = "horizontal" } },
}
local pageInstances, failPatchedPage = 0, false
_ug_loadedModules = {}
resolveutil = { resolve = function(path) return path end }
local function stubRequire(path)
    if path ~= SETTINGS_PAGE then return ui end
    if not _ug_loadedModules[path] then
        pageInstances = pageInstances + 1
        local instance = pageInstances
        local builtin = ug_require("/gui/main/builtin.lua")
        _ug_loadedModules[path] = function(params)
            if instance > 1 and failPatchedPage then error("changed by a game update") end
            return { page = instance, params = params, row = builtin.TextView { text = "FeverScaler Menu Key" } }
        end
    end
    return _ug_loadedModules[path]
end
ug_require = stubRequire
log = { warning = function(message) error(message) end, message = function() end }
dofile("settings/feverscaler.script.lua")

-- Walk closures rather than adding a test-only export to the game's mod API.
local seen = {}
local function find(fn, name)
    if seen[fn] then return end
    seen[fn] = true
    for n = 1, math.huge do
        local key, value = debug.getupvalue(fn, n)
        if not key then break end
        if key == name then return value end
        if type(value) == "function" then
            local found = find(value, name)
            if found then return found end
        end
    end
end
local makeGroup = assert(find(data().doReplace, "makeGroup"))

-- Before the plugin has written its state the group says so, and the script asks for it once.
assert(makeGroup().options[1].name == "FeverScaler did not report its settings")
assert(disk.request and writes == 1)
makeGroup()
assert(writes == 1, "the script asks only once")
userdata = { "state" }

local function reset(changes)
    disk = {
        fg = true, fgAvailable = true, frames = 3, maxFrames = 5,
        sr = true, srAvailable = true, scaleControl = true,
        mode = 2, preset = 12, previews = 1,
        menuKey = 49,
    }
    for k, v in pairs(changes or {}) do disk[k] = v end
    writes = 0
end
local function options()
    local group = makeGroup()
    local byName = {}
    for _, option in ipairs(group.options) do byName[option.name] = option end
    return byName
end
local function rebuild(opts)
    -- The game's settings page writes the current values back as it constructs its widgets.
    for _, option in pairs(opts) do
        if option.get and option.set then option.set(nil, option.get()) end
    end
end

reset()
local opts = options()
local multiplier = opts["Frame Generation Multiplier"]
assert(#multiplier.params == 5 and multiplier.params[5][3] == 5)
assert(not multiplier.disabled)
rebuild(opts)
assert(writes == 0, "rebuilding must not change any settings")
multiplier.set(nil, 5)
assert(disk.frames == 5 and writes == 1)
assert(disk.sr and disk.preset == 12, "FG edits must preserve SR settings")

reset({ maxFrames = 1, frames = 1 })
assert(#options()["Frame Generation Multiplier"].params == 1)
reset({ maxFrames = 0, frames = 1 })
assert(#options()["Frame Generation Multiplier"].params == 1)
reset({ maxFrames = 99 })
assert(#options()["Frame Generation Multiplier"].params == 5)

reset({ fg = false })
opts = options()
assert(opts["Frame Generation Multiplier"].disabled)

-- What cannot run on this PC is disabled and says why; the other feature stays usable.
local hags = "Hardware-accelerated GPU scheduling is off"
reset({ fgAvailable = false, fgReason = hags })
opts = options()
assert(opts["DLSS Frame Generation unavailable"].description == hags, "the full reason must reach the help panel")
assert(opts["DLSS Frame Generation"].disabled)
assert(opts["Frame Generation Multiplier"].disabled)
assert(not opts["DLSS Super Resolution"].disabled, "Super Resolution works without frame generation")
rebuild(opts)
assert(writes == 0)
local rtx = "not supported by this graphics card (NVIDIA RTX required)"
reset({ srAvailable = false, srReason = rtx })
opts = options()
assert(opts["DLSS Super Resolution unavailable"].description == rtx and opts["DLSS Super Resolution"].disabled)
assert(not opts["DLSS Frame Generation"].disabled)
reset({ fgAvailable = true, srReason = "", fgReason = "" })
for name in pairs(options()) do
    assert(not name:find("unavailable"), "no notes while everything runs")
end

-- The settings page is replaced by the extended one, which falls back to the game's own page if
-- it fails.
local replaced
local replacement = { ReplaceRecipe = function(original, page) replaced = { original = original, page = page } end }
reset()
data().doReplace(replacement)
assert(replaced, "the settings page is extended")
assert(ug_require == stubRequire)
local view = replaced.page({ tab = "graphics" })
assert(view.children[1].page == 2 and view.children[1].params.tab == "graphics")

-- The dev menu key's row in the extended page gets a button that takes the next key combination.
assert(options()["FeverScaler Menu Key"].type == "Label")
assert(_ug_loadedModules[SETTINGS_PAGE]({}).row.children == nil, "the game's own page instance is untouched")
local function keyRow()
    local row = replaced.page({ tab = "graphics" }).children[1].row
    assert(row.children[1].text == "FeverScaler Menu Key")
    return row.children[2].children[1]
end
local button = keyRow()
assert(button.content.text == "49" and button.meta.keyListener({}) == false,
    "the game needs a listener at widget creation; idle keys must pass through")
local function key(scancode, state)
    return button.meta.keyListener({ data = { getKey = function() return scancode end, dir = api.gui.key.State[state] } })
end
local function listen()
    keyRow().onClick()
    button = keyRow()
    assert(button.content.text == "Press Any Key Combination..." and button.meta.keyListener)
end
listen()
assert(key(40, "Up"), "keys go to the button, not the game") -- the Enter that clicked it, let go only now
key(228, "Pressed") -- right Ctrl
key(225, "Pressed") -- left Shift
key(65, "Pressed")
assert(writes == 0)
key(225, "Up") -- Shift let go first: still part of the combination
assert(disk.menuKey == 65 + 65536 + 131072 and disk.frames == 3 and writes == 1, "the key is saved on its own")
assert(keyRow().content.text == "Ctrl+Shift+65" and keyRow().meta.keyListener({}) == false)
listen()
key(226, "Pressed")
key(226, "Up") -- Alt let go before the key: not part of it
key(227, "Pressed") -- Windows key: ignored
key(9, "Pressed")
key(9, "Up")
assert(disk.menuKey == 9 and keyRow().content.text == "9")
listen()
key(41, "Pressed") -- Esc cancels
assert(disk.menuKey == 9 and writes == 2 and keyRow().content.text == "9")

failPatchedPage = true
local warnings = 0
log.warning = function() warnings = warnings + 1 end
view = replaced.page({ tab = "graphics" })
assert(view.children[1].original == replaced.original and view.children[1].params.tab == "graphics",
    "a failing extended page must show the game's own page")
failPatchedPage = false
view = replaced.page({ tab = "graphics" })
assert(view.children[1].original == replaced.original and warnings == 1, "the fallback sticks and is logged once")
print("PASS: state request, settings capabilities, x2-x6, persistence, dev menu key, "
    .. "unavailable features and settings page replacement")
