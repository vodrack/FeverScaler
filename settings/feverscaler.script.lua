-- FeverScaler settings in the game's own Settings > Graphics tab.
--
-- The settings page builds its tabs as plain tables inside one file-local function, with no
-- extension point. This script loads a second instance of the game's settings_page.tl whose
-- `ipairs` is a pass-through that adds one more group to the Graphics tab (and disables the game's
-- Resolution Scale slider while DLSS sets the render resolution), and registers that instance as
-- the replacement for the SettingsPage recipe (the game's recipe replacement API).
-- The page itself stays the game's own code, so it follows game updates. The page's option types
-- have no key binding for anything but the game's own actions, so that instance also gets a
-- `builtin` that adds a key button to the dev menu key's row.
--
-- The plugin serves this script to the game as base-game content (src/bridge.cpp), so it runs only
-- while the plugin is active, in the main menu and in every save. The plugin owns the settings and
-- mirrors them to <userdata>/feverscaler/state.lua, which this script reads and writes with
-- app.loadUserdata / app.saveUserdata.

local SETTINGS_PAGE = "::/gui/menu/settings_page.tl"
local MENU_KEY = "FeverScaler Menu Key" -- the dev menu key's row
local DIR, FILE = "feverscaler", "state"

local react = ug_require "::/gui/main/react.lua"
local builtin = ug_require "::/gui/main/builtin.lua"

local realIpairs = ipairs

-- ---- bridge ------------------------------------------------------------------------------------
local state = nil
local loadedAt = nil
local askedForState = false
local redrawPage = nil -- rebuilds the open settings page

local function refresh(force)
	local now = os.clock()
	if not force and loadedAt and now - loadedAt < 0.25 then
		return
	end
	loadedAt = now
	-- loadUserdata reports an error for a missing file, so look before loading
	for _, name in realIpairs(app.getAllUserdata(DIR)) do
		if name == FILE then
			local loaded = app.loadUserdata(DIR, FILE)
			if type(loaded) == "table" and loaded.frames ~= nil then
				state = loaded
			end
			return
		end
	end
	-- The plugin finds the user data folder by watching the game's files. Writing the file shows
	-- it the folder in case it guessed wrong; it answers with its state.
	if not askedForState then
		askedForState = true
		app.saveUserdata(DIR, FILE, { request = true })
	end
end

local function getter(key)
	return function()
		return state[key]
	end
end

local function setter(key)
	return function(_, value)
		if state[key] == value then
			return -- the page also calls set() with the current value on every rebuild
		end
		refresh(true) -- keep what the plugin changed meanwhile (its dev menu)
		state[key] = value
		app.saveUserdata(DIR, FILE, state)
		if redrawPage then
			redrawPage() -- the page only rebuilds itself when the game's own settings change
		end
	end
end

-- ---- the group ---------------------------------------------------------------------------------
-- DLSS Quality sets the render resolution: the plugin can (scaleControl) and SR is on and usable.
local function dlssSetsScale()
	return state.sr and state.srAvailable and state.scaleControl
end

local function makeGroup()
	refresh(false)
	if not state then
		local explanation = "See feverscaler.log in the game folder."
		return {
			title = "FeverScaler",
			description = explanation,
			feverscaler = true,
			options = {
				{ name = "FeverScaler did not report its settings", type = "Label", description = explanation },
			},
		}
	end

	local multipliers = {}
	for n = 1, math.min(5, math.max(1, state.maxFrames or 1)) do
		multipliers[#multipliers + 1] = { "x" .. tostring(n + 1), "x" .. tostring(n + 1), n }
	end
	local presetActive = dlssSetsScale()
	local fgAvailable = state.fgAvailable

	local group = {
		title = "FeverScaler",
		description = "DLSS Super Resolution and DLSS Frame Generation, provided by the FeverScaler plugin.",
		feverscaler = true,
		options = {
			{
				name = "DLSS Super Resolution",
				type = "BooleanToggleButtonGroup",
				get = getter("sr"),
				set = setter("sr"),
				disabled = not state.srAvailable,
				description = "Render at a lower resolution and let DLSS upscale the image to the screen. It replaces the game's own upscaling and anti-aliasing of the world view.\n\nNot available if DLSS could not be loaded on this graphics card.",
			},
			{
				name = "DLSS Quality",
				type = "ComboBox",
				get = getter("mode"),
				set = setter("mode"),
				params = {
					{ "dlaa", "DLAA (100%)", 1 },
					{ "quality", "Quality (67%)", 2 },
					{ "balanced", "Balanced (58%)", 3 },
					{ "performance", "Performance (50%)", 4 },
					{ "ultra_performance", "Ultra Performance (33%)", 5 },
				},
				disabled = not presetActive,
				description = "The resolution the game renders at before DLSS upscales it. It replaces the Resolution Scale slider.\n\nA higher value results in better quality, but requires more rendering performance.",
			},
			{
				name = "DLSS Model",
				type = "ComboBox",
				get = getter("preset"),
				set = setter("preset"),
				params = {
					{ "cnn", "CNN (DLSS 3, E)", 5 },
					{ "transformer", "Transformer (DLSS 4, K)", 11 },
					{ "transformer2_l", "Transformer 2 (DLSS 4.5, L)", 12 },
					{ "transformer2_m", "Transformer 2 (DLSS 4.5, M)", 13 },
				},
				disabled = not state.sr,
				description = "The DLSS model that upscales the image.\n\nTransformer 2 L is the sharpest and most stable, and the most expensive. Transformer 2 M looks close to L at a lower cost. CNN is the cheapest and softest.\n\nOn RTX 20 and 30 series cards, both Transformer 2 models cost about two to three times as much as Transformer.",
			},
			{
				name = "Preview Resolution",
				type = "ToggleButtonGroup",
				get = getter("previews"),
				set = setter("previews"),
				params = {
					{ "full", "Full", 0 },
					{ "preset", "DLSS Quality", 1 },
				},
				disabled = not presetActive,
				description = "Render resolution of the 3D previews in vehicle and station windows. DLSS does not upscale them.\n\nApplies the next time a preview window opens.",
			},
			{
				name = "DLSS Frame Generation",
				type = "BooleanToggleButtonGroup",
				get = getter("fg"),
				set = setter("fg"),
				disabled = not fgAvailable,
				description = "Show generated frames between the rendered ones for a higher frame rate.\n\nThe game's VSync and Anti-Aliasing are off while it is on. Use G-SYNC or FreeSync for tear-free frames.",
			},
			{
				name = "Frame Generation Multiplier",
				type = "ToggleButtonGroup",
				get = getter("frames"),
				set = setter("frames"),
				params = multipliers,
				disabled = not fgAvailable or not state.fg,
				description = "How many frames are shown for each rendered frame. Only multipliers reported by your GPU and driver are offered, up to x6. Native multi frame generation requires RTX 50 series; unlocks on older cards are experimental.",
			},
			{
				name = MENU_KEY, -- the page's builtin adds the key button (pageBuiltin)
				type = "Label",
				description = "The key that shows and hides the FeverScaler menu: the same options as here, plus frame rates and status.\n\nClick the button, then press a key, with Ctrl, Shift or Alt if you like. Esc cancels.",
			},
		},
	}
	-- Why a feature cannot run on this PC, as reported by the plugin, above the controls.
	local function note(title, reason)
		if type(reason) == "string" and reason ~= "" then
			-- Labels share the settings page's fixed-width name column; details belong in its help panel.
			table.insert(group.options, 1, { name = title, type = "Label", description = reason })
		end
	end
	if not fgAvailable then
		note("DLSS Frame Generation unavailable", state.fgReason)
	end
	if not state.srAvailable then
		note("DLSS Super Resolution unavailable", state.srReason)
	end
	return group
end

-- While DLSS Quality sets the render resolution, the game's Resolution Scale slider has no effect.
local function lockResolutionScale(groups)
	if not state or not dlssSetsScale() then
		return
	end
	for _, group in realIpairs(groups) do
		for _, option in realIpairs(group.options or {}) do
			if option.key == "resolutionScale" then
				option.disabled = true
				option.description = "Set by DLSS Quality while DLSS Super Resolution is on."
			end
		end
	end
end

local function addGroup(groups)
	for _, group in realIpairs(groups) do
		if group.feverscaler then
			return
		end
	end
	local ok, group = pcall(makeGroup)
	if ok then
		table.insert(groups, 2, group) -- after "Window Settings"
		lockResolutionScale(groups)
	else
		log.warning("FeverScaler: could not build the settings group: " .. tostring(group))
	end
end

-- ---- the menu key button -----------------------------------------------------------------------
-- A menu key in the state file is an SDL scancode (what the game's key events report) plus 65536
-- for Ctrl, 131072 for Shift and 262144 for Alt; the plugin turns it into a Windows key.
local MODIFIERS = { [224] = 65536, [228] = 65536, [225] = 131072, [229] = 131072, [226] = 262144, [230] = 262144 }
local ESC, WINDOWS_KEYS = 41, { [227] = true, [231] = true }

-- A menu key as the game names keys, in the game's language.
local function keyName(key)
	if not key or key == 0 then
		return _("<None>")
	end
	local combo = api.type.KeyComboDef.new()
	combo:setKeyScancode(key % 65536)
	for scancode, bit in pairs(MODIFIERS) do
		if scancode < 227 and math.floor(key / bit) % 2 == 1 then -- the left-hand ones
			combo:addModifierKey(scancode)
		end
	end
	return api.type.KeyCombo.new(combo):toString()
end

-- One key event while the button listens, collected in `held`: nil while the combination is not
-- complete, 0 for Esc (cancel), else the menu key: a key with the modifiers held when it went down,
-- complete when the first key is let go.
local function captureKey(held, scancode, pressed)
	if scancode == ESC then
		return 0
	elseif WINDOWS_KEYS[scancode] then
		return nil
	elseif not pressed then
		if held.key then
			return held.key
		end
		held[scancode] = nil -- a modifier let go before the key, or the key that clicked the button
	elseif MODIFIERS[scancode] then
		held[scancode] = true
	else
		local bits, key = {}, scancode
		for code in pairs(held) do
			local bit = MODIFIERS[code]
			if bit and not bits[bit] then -- left and right Ctrl count once
				bits[bit] = true
				key = key + bit
			end
		end
		held.key = key
	end
	return nil
end

-- Shows the menu key; clicked, it takes the next key combination, like the game's key bindings.
local KeyButton = react.RegisterRecipe("FeverScalerKeyButton", function()
	local listening = react.useState(false)
	local held = react.useRef(nil)
	react.onMouseEvent(function(evt)
		if listening:old() and evt.type == api.gui.mouse.Event.Type.PressedOutside then
			listening:set(false)
		end
		return false
	end)
	local button = builtin.Button {
		meta = {
			class = "valueItem, key-combo",
			-- The game installs keyboard routing when the widget is created. Keep a listener
			-- present while idle as well, just like its native KeyListenerButton recipe.
			keyListener = function(evt)
				if not listening:old() then
					return false
				end
				local key = captureKey(held:get(), evt.data:getKey(), evt.data.dir == api.gui.key.State.Pressed)
				if key then
					listening:set(false)
					if key ~= 0 then
						setter("menuKey")(nil, key)
					end
				end
				return true -- keys pressed for the binding do nothing in the game
			end,
		},
		content = builtin.TextView {
			meta = { class = "font-scale-body" .. (listening:old() and ",changing" or "") },
			text = listening:old() and _("Press Any Key Combination...") or keyName(state.menuKey),
		},
		onClick = function()
			held:set({})
			listening:set(true)
		end,
	}
	-- Custom recipes must return a layout; a bare Button fails during the game's transform.
	return builtin.BoxLayout { children = { button } }
end)

-- builtin for the settings page instance: the menu key's row, a Label, gets the key button next to
-- its text, laid out like the game's key binding rows.
local pageBuiltin = setmetatable({
	TextView = function(params)
		if params.text ~= MENU_KEY then
			return builtin.TextView(params)
		end
		return builtin.BoxLayout {
			orientation = builtin.type.Orientation.Horizontal,
			children = { builtin.TextView(params), KeyButton {} },
		}
	end,
}, { __index = builtin })

-- ipairs for the settings page instance: the first table it sees that is the tab list, or the
-- Graphics tab's group list, gets the FeverScaler group.
local function settingsIpairs(t)
	if type(t) == "table" then
		local first = t[1]
		if type(first) == "table" then
			if first.groups ~= nil and first.key ~= nil then
				for _, tab in realIpairs(t) do
					if tab.key == "graphics" then
						addGroup(tab.groups)
					end
				end
			elseif first.options ~= nil then
				local option = first.options[1]
				if type(option) == "table" and option.key == "screenMode" then
					addGroup(t)
				end
			end
		end
	end
	return realIpairs(t)
end

-- A second instance of the game's settings page module. Teal-compiled modules take `ipairs` from
-- the globals, and their modules from ug_require, once, when they load.
local function loadPatchedSettingsPage()
	local resolved = resolveutil.resolve(SETTINGS_PAGE, "")
	local original = _ug_loadedModules[resolved]
	_ug_loadedModules[resolved] = nil
	package.loaded[resolved] = nil
	local realRequire = ug_require
	ipairs = settingsIpairs
	ug_require = function(path)
		if path:match("/gui/main/builtin%.lua$") then
			return pageBuiltin
		end
		return realRequire(path)
	end
	local ok, patched = pcall(realRequire, SETTINGS_PAGE)
	ipairs = realIpairs
	ug_require = realRequire
	_ug_loadedModules[resolved] = original
	package.loaded[resolved] = original
	if not ok then
		error(patched, 0)
	end
	return patched
end

local function replaceSettingsPage(replacementApi)
	local original = ug_require(SETTINGS_PAGE)
	local patched = loadPatchedSettingsPage()
	if patched == original then
		error("the settings page was not loaded a second time", 0)
	end
	local failed = false -- the extended page failed once: the game's own page from then on
	local page = react.RegisterRecipe("FeverScalerSettingsPage", function(params)
		local redraws = react.useState(0) -- declared on every render, whichever page follows
		if not failed then
			local ok, result = pcall(function()
				redrawPage = function()
					redraws:set(redraws:old() + 1)
				end
				-- a changed parameter makes the page rebuild
				local pageParams = { feverscalerRedraws = redraws:old() }
				for k, v in pairs(params) do
					pageParams[k] = v
				end
				return builtin.BoxLayout { children = { patched(pageParams) } }
			end)
			if ok then
				return result
			end
			failed = true
			redrawPage = nil
			log.warning("FeverScaler: the extended settings page failed, showing the game's own page: " .. tostring(result))
		end
		return builtin.BoxLayout { children = { react.CallOriginalRecipe(original, params) } }
	end)
	replacementApi.ReplaceRecipe(original, page)
end

function data()
	return {
		doReplace = function(replacementApi)
			local ok, err = pcall(replaceSettingsPage, replacementApi)
			if ok then
				log.message("FeverScaler: settings page extended")
			else
				log.warning("FeverScaler: could not extend the settings page: " .. tostring(err))
			end
		end,
	}
end
