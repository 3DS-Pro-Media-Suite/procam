--[[
  procam_import.lua: the "3DS import" module in darktable's lighttable. It fetches Pro Cam's photos from the
  3DS over FTP, imports them and deletes them on the 3DS once darktable has them. It can also run neural
  restore's raw denoise on the new raws.

  procam_ftp.py next to this file does the FTP work (Python 3, standard library only). Load this file from
  darktable's luarc:
      package.path = package.path .. ";<path to procam>/tools/darktable/?.lua"
      require "procam_import"

  A shot is deleted on the 3DS only when every step for it worked: its files downloaded with the size (and
  CRC) the 3DS reports, its DNG decoded by darktable's raw loader (darktable-rs-identify), and darktable
  imported it. Anything that fails stays on the 3DS and is listed in the module. When the connection is gone
  for good nothing is deleted, and the next import picks up where this one stopped. All pictures of a shot
  are imported as one group. Shots without pictures are only copied and deleted.

  Raw denoise uses neural restore's own settings (strength, output folder, add to library).
]]

local darktable = require "darktable"
local dtutils = require "lib/dtutils"

local MODULE_NAME <const> = "procam_import"
local ON_WINDOWS <const> = darktable.configuration.running_os == "windows"
local PATH_SEPARATOR <const> = ON_WINDOWS and "\\" or "/"
-- The chunk name is "@" followed by this file's path.
local SCRIPT_FOLDER <const> = debug.getinfo(1, "S").source:sub(2):match("^(.*)[/\\]") or "."
local HELPER_PATH <const> = SCRIPT_FOLDER .. PATH_SEPARATOR .. "procam_ftp.py"
-- Tried in order when the "python" preference is empty. py is the launcher that the Windows installer adds;
-- it finds Python 3 wherever it is installed.
local PYTHON_COMMANDS <const> = ON_WINDOWS and {"python", "py -3"} or {"python3", "python"}
local NULL_DEVICE <const> = ON_WINDOWS and "NUL" or "/dev/null"
local RAW_LOADER_PATH <const> = "C:\\Program Files\\darktable\\bin\\darktable-rs-identify.exe"

local BYTES_PER_KB <const> = 1024
local BYTES_PER_MB <const> = BYTES_PER_KB * BYTES_PER_KB
local SECONDS_PER_MINUTE <const> = 60
local SECONDS_PER_HOUR <const> = 3600
local SECONDS_STEP <const> = 5 -- times under a minute round up to this
local PROGRESS_BAR_CELLS <const> = 24
local MAX_NOTES <const> = 30
local PANEL_POSITION <const> = 100

-- Shares of the progress bar: download 0-85 %, import 85-92 %, delete 92-100 %.
local BAR <const> = {
  download_share = 0.85,
  import_start = 0.85,
  import_share = 0.07,
  delete_start = 0.92,
  delete_share = 0.08,
}

dtutils.check_min_api_version("7.0.0", MODULE_NAME)

-- A bool preference has a "_set" twin: an unset bool reads as false, which would hide its default.
local function read_preference(key, kind, default)
  local value = darktable.preferences.read(MODULE_NAME, key, kind)
  if kind == "string" and (value == nil or value == "") then
    return default
  end
  if kind == "bool" and not darktable.preferences.read(MODULE_NAME, key .. "_set", "bool") then
    return default
  end
  return value
end

local function write_preference(key, kind, value)
  darktable.preferences.write(MODULE_NAME, key, kind, value)
  if kind == "bool" then
    darktable.preferences.write(MODULE_NAME, key .. "_set", "bool", true)
  end
end

local function file_exists(path)
  local file = io.open(path, "rb")
  if file then
    file:close()
  end
  return file ~= nil
end

local function first_existing_file(paths, fallback)
  for _, path in ipairs(paths) do
    if file_exists(path) then
      return path
    end
  end
  return fallback
end

local function quote(text)
  return '"' .. text .. '"'
end

-- The "python" preference, else the first of PYTHON_COMMANDS that runs.
local function python_command()
  local configured = read_preference("python", "string", "")
  if configured ~= "" then
    return quote(configured)
  end
  for _, command in ipairs(PYTHON_COMMANDS) do
    if os.execute(command .. " --version >" .. NULL_DEVICE .. " 2>&1") then
      return command
    end
  end
  return PYTHON_COMMANDS[1]
end

local function split_fields(line)
  local fields = {}
  local trimmed = line:gsub("\r$", "")
  for field in (trimmed .. "\t"):gmatch("([^\t]*)\t") do
    fields[#fields + 1] = field
  end
  return fields
end

-- Runs the helper and calls on_line(fields, line) for each line it prints, as it prints it. On Windows the
-- command goes through a batch file: cmd.exe mangles command lines with more than one quoted part.
local function run_helper(arguments, on_line)
  local python = python_command()
  local parts = {python, "-X", "utf8", quote(HELPER_PATH)}
  for _, argument in ipairs(arguments) do
    parts[#parts + 1] = quote(argument)
  end
  local command = table.concat(parts, " ") .. " 2>&1"
  local batch_path
  if ON_WINDOWS then
    batch_path = darktable.configuration.tmp_dir .. PATH_SEPARATOR .. "procam_import.bat"
    local batch_file = io.open(batch_path, "w")
    if not batch_file then
      return false, "could not write " .. batch_path
    end
    -- Code page 65001 is UTF-8.
    batch_file:write("@echo off\r\nchcp 65001>nul\r\n" .. command .. "\r\n")
    batch_file:close()
    command = quote(batch_path)
  end
  local pipe = io.popen(command, "r")
  if not pipe then
    return false, "could not start " .. python
  end
  for line in pipe:lines() do
    on_line(split_fields(line), line)
  end
  pipe:close()
  if batch_path then
    os.remove(batch_path)
  end
  return true
end

local widgets = {}

widgets.host = darktable.new_widget("entry"){
  text = read_preference("host", "string", ""),
  placeholder = "3DS address",
  tooltip = "the 3DS's IP address (Pro Cam shows it with the FTP server on); port 5021",
}

widgets.destination = darktable.new_widget("file_chooser_button"){
  title = "photo library folder",
  is_directory = true,
  value = read_preference("dest", "string", ""),
  tooltip = "shots go to <this folder>\\<year>\\<year>-<month>-<day> (the day the 3DS took them)",
}

widgets.delete = darktable.new_widget("check_button"){
  label = "delete from the 3DS after import",
  value = read_preference("delete", "bool", true),
  tooltip = "a shot is deleted only once it is downloaded complete, checked and in the library",
}

widgets.denoise = darktable.new_widget("check_button"){
  label = "raw denoise new raws",
  value = read_preference("denoise", "bool", true),
  tooltip = "run neural restore's raw denoise on the newly imported DNGs"
    .. " (its strength and output settings apply)",
}

-- The status: the current step, a bar for the whole run and its numbers. Problems collect in the notes.
widgets.stage = darktable.new_widget("label"){ label = "", halign = "start", ellipsize = "end" }
widgets.bar = darktable.new_widget("label"){ label = "", halign = "start" }
widgets.detail = darktable.new_widget("label"){ label = "", halign = "start", ellipsize = "end" }
widgets.notes = darktable.new_widget("text_view"){ editable = false, text = "" }

local function remember_settings()
  write_preference("host", "string", widgets.host.text)
  write_preference("dest", "string", widgets.destination.value or "")
  write_preference("delete", "bool", widgets.delete.value)
  write_preference("denoise", "bool", widgets.denoise.value)
end

local function format_bytes(count)
  if count < BYTES_PER_KB then
    return string.format("%d B", count)
  end
  if count < BYTES_PER_MB then
    return string.format("%.0f KB", count / BYTES_PER_KB)
  end
  return string.format("%.1f MB", count / BYTES_PER_MB)
end

local function format_duration(seconds)
  if seconds < SECONDS_PER_MINUTE then
    return string.format("%d s", math.ceil(seconds / SECONDS_STEP) * SECONDS_STEP)
  end
  if seconds < SECONDS_PER_HOUR then
    return string.format("%d min", math.ceil(seconds / SECONDS_PER_MINUTE))
  end
  return string.format("%.1f h", seconds / SECONDS_PER_HOUR)
end

local function show_progress(fraction, job)
  fraction = math.max(0, math.min(1, fraction))
  local full_cells = math.floor(fraction * PROGRESS_BAR_CELLS + 0.5)
  local empty_cells = PROGRESS_BAR_CELLS - full_cells
  local percent = string.format("  %d %%", math.floor(fraction * 100))
  widgets.bar.label = string.rep("█", full_cells) .. string.rep("░", empty_cells) .. percent
  if job then
    job.percent = fraction
  end
end

local notes = {}

local function note(message)
  notes[#notes + 1] = message
  while #notes > MAX_NOTES do
    table.remove(notes, 1)
  end
  widgets.notes.text = table.concat(notes, "\n")
  darktable.print_log("[procam_import] " .. message)
end

local function reset_status()
  notes = {}
  widgets.notes.text = ""
  widgets.detail.label = ""
  widgets.stage.label = ""
  show_progress(0)
end

local function show_film_roll(path)
  local rule = darktable.gui.libs.collect.new_rule()
  rule.mode = "DT_LIB_COLLECT_MODE_AND"
  rule.data = path
  rule.item = "DT_COLLECTION_PROP_FILMROLL"
  darktable.gui.libs.collect.filter({rule})
end

-- The folder and everything below it: darktable's folder rule with a trailing *.
local function show_folder_tree(path)
  local rule = darktable.gui.libs.collect.new_rule()
  rule.mode = "DT_LIB_COLLECT_MODE_AND"
  rule.data = path .. "*"
  rule.item = "DT_COLLECTION_PROP_FOLDERS"
  darktable.gui.libs.collect.filter({rule})
end

-- darktable.gui.action returns NaN for an action that does not exist.
local function is_nan(value)
  return value ~= value
end

-- Neural restore works on the selected images of the current collection. So each film roll with new raws is
-- shown, its raws selected and the "process" button pressed. Each roll becomes a background job.
local function denoise(images)
  local images_by_roll = {}
  local roll_order = {}
  for _, image in ipairs(images) do
    local roll = image.path
    if not images_by_roll[roll] then
      images_by_roll[roll] = {}
      roll_order[#roll_order + 1] = roll
    end
    table.insert(images_by_roll[roll], image)
  end
  if darktable.gui.current_view() ~= darktable.gui.views.lighttable then
    darktable.gui.current_view(darktable.gui.views.lighttable)
  end
  local page_result = darktable.gui.action("lib/neural_restore/page", "raw denoise", "activate", 1.0)
  if is_nan(page_result) then
    note("Raw denoise: neural restore not found (is the module shown in lighttable?)")
    return 0
  end
  local queued = 0
  for _, roll in ipairs(roll_order) do
    show_film_roll(roll)
    darktable.gui.selection(images_by_roll[roll])
    local process_result = darktable.gui.action("lib/neural_restore/process", "", "activate", 1.0)
    if is_nan(process_result) then
      note("Raw denoise: could not press process for " .. roll)
    else
      queued = queued + #images_by_roll[roll]
    end
  end
  return queued
end

local function retry_text(fields)
  return string.format("Connection trouble (%s), trying again in %s s", fields[3] or "?", fields[4] or "?")
end

local function show_transfer(run, job, fields)
  local run_done = tonumber(fields[4]) or 0
  local run_total = tonumber(fields[5]) or 0
  local rate = tonumber(fields[6]) or 0
  if run_total > 0 then
    show_progress(run_done / run_total * BAR.download_share, job)
  end
  local text = format_bytes(run_done) .. " of " .. format_bytes(run_total)
  if rate > 0 then
    text = text .. "  ·  " .. format_bytes(rate) .. "/s"
  end
  if rate > 0 and run_done < run_total then
    text = text .. "  ·  about " .. format_duration((run_total - run_done) / rate) .. " left"
  end
  if run.retries > 0 then
    text = text .. string.format("  ·  %d reconnect(s)", run.retries)
  end
  widgets.detail.label = text
end

-- One line of the helper's fetch output. The protocol is described in procam_ftp.py.
local function on_fetch_line(run, job, fields, line)
  local kind = fields[1]
  if kind == "COUNT" then
    run.total = tonumber(fields[2]) or 0
    if run.total == 0 then
      widgets.stage.label = "No new shots on the 3DS"
    else
      widgets.stage.label = string.format("%d shot(s) on the 3DS", run.total)
      widgets.detail.label = format_bytes(tonumber(fields[3]) or 0) .. " to download"
    end
  elseif kind == "FILE" then
    widgets.stage.label = string.format("Downloading %s of %d: %s", fields[2], run.total, fields[3])
  elseif kind == "BYTES" then
    show_transfer(run, job, fields)
  elseif kind == "RETRY" then
    run.retries = run.retries + 1
    widgets.detail.label = retry_text(fields)
  elseif kind == "SHOT" then
    run.shots[#run.shots + 1] = {
      status = fields[2],
      path = fields[3],
      is_raw = fields[4] == "1",
      remote_files = fields[5] or "",
      group_pictures = fields[6] or "",
    }
  elseif kind == "SKIP" then
    run.skipped = run.skipped + 1
    note("Left on the 3DS: " .. fields[2] .. " (" .. (fields[3] or "") .. ")")
  elseif kind == "FAIL" then
    run.failed = run.failed + 1
    note("Failed, left on the 3DS: " .. fields[2] .. ": " .. (fields[3] or ""))
  elseif kind == "ERROR" then
    run.link_lost = true
    note("Connection lost: " .. (fields[2] or ""))
  elseif line ~= "" then
    -- A Python error or similar.
    note(line)
  end
end

local function fetch_shots(run, host, destination, verify_tool, job)
  widgets.stage.label = "Connecting to " .. host .. " ..."
  local arguments = {"fetch", "--host", host, "--dest", destination}
  if verify_tool then
    arguments[#arguments + 1] = "--verify"
    arguments[#arguments + 1] = verify_tool
  else
    note("darktable-rs-identify.exe not found: DNGs are not test-decoded before deleting")
  end
  local ok, problem = run_helper(arguments, function(fields, line)
    on_fetch_line(run, job, fields, line)
  end)
  if not ok then
    note(problem)
    run.link_lost = true
  end
end

local function add_remote_files(list, shot)
  for name in shot.remote_files:gmatch("[^|]+") do
    list[#list + 1] = name
  end
end

-- The shot's other pictures, such as a RAW+JPG shot's JPG, join the image's group.
local function import_group(image, shot)
  for path in shot.group_pictures:gmatch("[^|]+") do
    local picture = darktable.database.import(path)
    if picture then
      picture:group_with(image)
    end
  end
  if shot.group_pictures ~= "" then
    image:make_group_leader()
  end
end

-- Imports the fetched shots. Collects the new raws and the remote files of every shot that made it.
local function import_shots(run, job)
  for index, shot in ipairs(run.shots) do
    widgets.stage.label = string.format("Importing %d of %d", index, #run.shots)
    show_progress(BAR.import_start + BAR.import_share * (index - 1) / #run.shots, job)
    if shot.path == "" then
      -- No picture in it (a lone TXT): downloaded and checked, nothing for darktable to show.
      run.copied = run.copied + 1
      add_remote_files(run.to_delete, shot)
    else
      local image = darktable.database.import(shot.path)
      if image then
        run.imported = run.imported + 1
        import_group(image, shot)
        if shot.is_raw and shot.status == "new" then
          run.new_raws[#run.new_raws + 1] = image
        end
        add_remote_files(run.to_delete, shot)
      else
        run.failed = run.failed + 1
        note("darktable did not import " .. shot.path .. ": left on the 3DS")
      end
    end
  end
end

local function on_delete_line(run, job, fields, line)
  local kind = fields[1]
  if kind == "DELETED" then
    run.deleted = run.deleted + 1
    widgets.stage.label = string.format("Deleting on the 3DS: %d of %d files", run.deleted, #run.to_delete)
    show_progress(BAR.delete_start + BAR.delete_share * run.deleted / #run.to_delete, job)
  elseif kind == "NOTDELETED" then
    note("Not deleted on the 3DS: " .. fields[2] .. " (" .. (fields[3] or "") .. ")")
  elseif kind == "RETRY" then
    widgets.detail.label = retry_text(fields)
  elseif kind == "ERROR" then
    note("Connection lost: " .. (fields[2] or ""))
  elseif line ~= "" then
    note(line)
  end
end

local function delete_on_3ds(run, host, job)
  if run.link_lost then
    note("Nothing deleted on the 3DS: the connection is gone. "
      .. "The next import recognises these shots and deletes them.")
    return
  end
  local arguments = {"delete", "--host", host}
  for _, name in ipairs(run.to_delete) do
    arguments[#arguments + 1] = name
  end
  widgets.detail.label = ""
  run_helper(arguments, function(fields, line)
    on_delete_line(run, job, fields, line)
  end)
end

local function show_summary(run)
  local summary = string.format("Imported %d shot(s)", run.imported)
  if run.copied > 0 then
    summary = summary .. string.format(", copied %d without pictures", run.copied)
  end
  if widgets.delete.value and run.deleted > 0 then
    summary = summary .. string.format(", deleted %d item(s) on the 3DS", run.deleted)
  end
  if run.failed > 0 then
    summary = summary .. string.format(", %d failed", run.failed)
  end
  if run.link_lost and run.imported == 0 and run.failed == 0 then
    summary = "Could not reach the 3DS"
  end
  widgets.stage.label = summary
  if run.skipped > 0 then
    widgets.detail.label = string.format("%d item(s) left on the 3DS, see below", run.skipped)
  else
    widgets.detail.label = ""
  end
  darktable.print(summary)
end

-- Ends on the day folder of the last shot, with the shot folders in it.
local function show_last_day(shots)
  local last_path
  for _, shot in ipairs(shots) do
    if shot.path ~= "" then
      last_path = shot.path
    end
  end
  if not last_path then
    return
  end
  local folder = last_path:match("^(.*)[/\\]")
  -- A picture inside a shot folder (IMG_0042/): its parent is the day folder.
  if folder:match("[/\\]IMG_%d+[^/\\]*$") then
    folder = folder:match("^(.*)[/\\]")
  end
  show_folder_tree(folder)
end

local function import_from_3ds()
  remember_settings()
  reset_status()
  local host = widgets.host.text
  local destination = (widgets.destination.value or ""):gsub("[\\/]+$", "")
  if host == "" or destination == "" then
    widgets.stage.label = "Set the 3DS address and the photo folder first"
    return
  end
  local verify_tool = first_existing_file({read_preference("verify", "string", ""), RAW_LOADER_PATH}, nil)
  local job = darktable.gui.create_job("3DS import", true)
  local run = {
    total = 0,
    shots = {},
    failed = 0,
    skipped = 0,
    link_lost = false,
    retries = 0,
    imported = 0,
    copied = 0,
    new_raws = {},
    to_delete = {},
    deleted = 0,
  }
  fetch_shots(run, host, destination, verify_tool, job)
  import_shots(run, job)
  if widgets.delete.value and #run.to_delete > 0 then
    delete_on_3ds(run, host, job)
  end
  show_progress(1, job)
  job.valid = false
  show_summary(run)
  if widgets.denoise.value and #run.new_raws > 0 then
    local queued = denoise(run.new_raws)
    if queued > 0 then
      widgets.detail.label =
        string.format("Raw denoise queued for %d image(s) (see the progress bottom left)", queued)
    end
  end
  show_last_day(run.shots)
end

local busy = false

local function import_clicked()
  if busy then
    return
  end
  busy = true
  local ok, problem = pcall(import_from_3ds)
  if not ok then
    note("Error: " .. tostring(problem))
  end
  busy = false
end

widgets.button = darktable.new_widget("button"){
  label = "import from 3DS",
  tooltip = "download the shots from Pro Cam's FTP server, import them,"
    .. " then (optionally) delete and raw denoise",
  clicked_callback = function()
    import_clicked()
  end,
}

local main_widget = darktable.new_widget("box"){
  orientation = "vertical",
  darktable.new_widget("section_label"){ label = "3DS" },
  widgets.host,
  widgets.destination,
  widgets.delete,
  widgets.denoise,
  widgets.button,
  widgets.stage,
  widgets.bar,
  widgets.detail,
  widgets.notes,
}

-- Registers the module once darktable's GUI is up: luarc sets darktable_gui_safe on the first switch to
-- lighttable.
local installed = false

local function install()
  if installed then
    return
  end
  local views = {[darktable.gui.views.lighttable] = {"DT_UI_CONTAINER_PANEL_LEFT_CENTER", PANEL_POSITION}}
  darktable.register_lib(MODULE_NAME, "3DS import", true, false, views, main_widget, nil, nil)
  installed = true
end

if darktable.gui.current_view().id == "lighttable" and darktable_gui_safe then
  install()
else
  darktable.register_event(MODULE_NAME, "view-changed", function(event, old_view, new_view)
    if new_view.id == "lighttable" then
      install()
    end
  end)
end

return {}
