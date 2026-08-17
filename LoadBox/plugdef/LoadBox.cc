/*
 * LoadBox.cc
 *
 *
 * SPDX-License-Identifier:  BSD-3-Clause
 * 
 * Copyright (C) 2025 brummer <brummer@web.de>
 */

#include <atomic>
#include <errno.h>
#include <unistd.h>
#include <signal.h>

#include <iostream>
#include <fstream>
#include <cstring>
#include <string>
#include <cmath>
#include <mutex>

#include <locale.h>

#include "engine.h"
#include "ParallelThread.h"
#include "Parameter.h"
#include "PluginAPI.h"
#ifndef HEADLESS
#define CLAPPLUG
#include "LoadBox.c"
#endif

class LoadBox
{
public:
  #ifndef HEADLESS
    Widget_t*               TopWin;
  #endif
    Params                  param;

    // string property ids. These share an id space with the parameter indices
    // in some hosts (Sushi registers both through Processor::register_parameter),
    // so they start well clear of registerParameters() below.
    enum {
        IrFileLeftPropertyId  = 100,
        IrFileRightPropertyId = 101
    };

    LoadBox() : engine(), param() {
        workToDo.store(false, std::memory_order_release);
        propListener.store(nullptr, std::memory_order_release);
        for (int i = 0; i < 2; i++) {
            pendingSet[i] = false;
            // matches the engine's own initial state, so the first service
            // tick does not report a change that never happened
            publishedFile[i] = "None";
        }
    #ifndef HEADLESS
        ui = (X11_UI*)malloc(sizeof(X11_UI));
        ui->private_ptr = NULL;
        ui->need_resize = 1;
        ui->loop_counter = 4;
        ui->uiKnowSampleRate = false;
        ui->setVerbose = false;
        ui->uiSampleRate = 0;
        ui->f_index = 0;
        title = "IR Loader";
        firstLoop = true;
        p = 0;
    #endif
        registerParameters();
    #ifndef HEADLESS
        for(int i = 0;i<CONTROLS;i++)
            ui->widget[i] = NULL;
    #endif
    }

    ~LoadBox() {
        // stop the service thread first: it touches the engine, which is
        // destroyed as soon as this body returns
        service.stop();
    #ifndef HEADLESS
        fetch.stop();
        free(ui->private_ptr);
        free(ui);
    #endif
    }

    void registerParameters() {
        //                  name             group   min, max, def, step   value              isStepped  type
        param.registerParam("Enable",         "Global", 0,1,1,1,     (void*)&engine.bypass,        true,  IS_UINT);

        param.registerParam("IR Out Gain L",  "IR",    -20,20,0,0.1, (void*)&engine.IRoutputGain,   false, Is_FLOAT);
        param.registerParam("IR Out Gain R",  "IR",    -20,20,0,0.1, (void*)&engine.IRoutputGain1,  false, Is_FLOAT);

        param.registerParam("IR Mode",        "IR",     0,1,0,1,     (void*)&engine.IRmode,         true,  IS_UINT);
        param.registerParam("IR Mix",         "IR",     0,1,0.5,0.01,(void*)&engine.IRmix,          false, Is_FLOAT);
        param.registerParam("Master",         "IR",    -20,20,0,0.1, (void*)&engine.MasterOutGain,  false, Is_FLOAT);
    }

    #ifndef HEADLESS
    void startGui(Window window) {
        main_init(&ui->main);
        if (ui->main.hdpi > 1.6) ui->main.hdpi = 1.6;
        set_custom_theme(ui);
        int w = 1;
        int h = 1;
        plugin_set_window_size(&w,&h,"clap_plugin");
        #if defined(_WIN32)
        TopWin  = create_window(&ui->main, (HWND) window, 0, 0, w, h);
        #else
        TopWin  = create_window(&ui->main, (Window) window, 0, 0, w, h);
        #endif
        TopWin->flags |= HIDE_ON_DELETE;
        ui->win = create_widget(&ui->main, TopWin, 0, 0, w, h);
        ui->win->scale.gravity = NORTHWEST;
        widget_set_title(TopWin, title.c_str());
        ui->win->parent_struct = ui;
        ui->win->private_struct = (void*)this;
        plugin_create_controller_widgets(ui,"clap_plugin");
        fetch.startTimeout(60);
        fetch.set<LoadBox, &LoadBox::runGui>(this);
    }

    void startGui() {
        main_init(&ui->main);
        if (ui->main.hdpi > 1.6) ui->main.hdpi = 1.6;
        set_custom_theme(ui);
        int w = 1;
        int h = 1;
        plugin_set_window_size(&w,&h,"clap_plugin");
        TopWin  = create_window(&ui->main, os_get_root_window(&ui->main, IS_WINDOW), 0, 0, w, h);
        TopWin->flags |= HIDE_ON_DELETE;
        ui->win = create_widget(&ui->main, TopWin, 0, 0, w, h);
        widget_set_title(TopWin, title.c_str());
        ui->win->parent_struct = ui;
        ui->win->private_struct = (void*)this;
        plugin_create_controller_widgets(ui,"clap_plugin");
        fetch.startTimeout(60);
        fetch.set<LoadBox, &LoadBox::runGui>(this);
    }

    void showGui() {
        engine._notify_ui.store(true, std::memory_order_release);
        getEngineValues();
        widget_show_all(TopWin);
        firstLoop = true;
    }

    void setParent(Window window) {
        #if defined(_WIN32)
        SetParent(TopWin->widget, (HWND) window);
        #else
        XReparentWindow(ui->main.dpy, TopWin->widget, (Window) window, 0, 0);
        #endif
        p = window;
    }

    void checkParentWindowSize(int width, int height) {
        #if defined (IS_VST2)
        if (!p) return;
        int host_width = 1;
        int host_height = 1;
        #if defined(_WIN32)
        RECT rect;
        if (GetClientRect((HWND) p, &rect)) {
            host_width  = rect.right - rect.left;
            host_height = rect.bottom - rect.top;
        }
        #else
        XWindowAttributes attrs;
        if (XGetWindowAttributes(ui->main.dpy, p, &attrs)) {
            host_width  = attrs.width;
            host_height = attrs.height;
        }
        #endif
        if ((host_width != width && host_width != 1) ||
            (host_height != height && host_height != 1)) {
            os_resize_window(ui->main.dpy, TopWin, host_width, host_height);
        }
        #endif
    }

    void hideGui() {
        widget_hide(TopWin);
        firstLoop = false;
    }

    void quitGui() {
        fetch.stop();
        cleanup();
        main_quit(&ui->main);
    }

    void runGui() {
        checkEngine();
        if (firstLoop) {
            checkParentWindowSize(TopWin->width, TopWin->height);
            firstLoop = false;
        }
        if (param.paramChanged.load(std::memory_order_acquire)) {
            getEngineValues();
            param.paramChanged.store(false, std::memory_order_release);
        }
        run_embedded(&ui->main);
    }

    // check output ports from engine
    void checkEngine() {
        // when serviceEngine() has just handed work to the loader thread, skip
        // the branch below: it clears engine._cd, which the loader is reading
        if (serviceEngine()) return;
        if (engine._notify_ui.load(std::memory_order_acquire)) {
            engine._notify_ui.store(false, std::memory_order_release);
            X11_UI_Private_t *ps = (X11_UI_Private_t*)ui->private_ptr;
            get_file(engine.ir_file, &ps->ir);
            get_file(engine.ir_file1, &ps->ir1);
            expose_widget(ui->win);
            engine._cd.store(0, std::memory_order_release);
        }
    }

    Xputty *getMain() {
        return &ui->main;
    }

    void enableEngine(int on) {
        adj_set_value(ui->widget[9]->adj, static_cast<float>(on));
    }
  #endif

    irloader::Engine *getEngine() {
        return &engine;
    }

    void initEngine(uint32_t rate, int32_t prio, int32_t policy) {
        engine.init(rate, prio, policy);
        engine.bypass = 1;
        param.setParamDirty(0 , true);
        param.controllerChanged.store(true, std::memory_order_release);
    #ifdef HEADLESS
        // Without a GUI there is no timer thread to feed the loader thread and
        // report back what it loaded, so run one here. Deliberately a plain
        // thread at default priority - it does file IO by proxy and must never
        // be scheduled as real time. In the GUI build the existing fetch loop
        // covers this, via checkEngine() below.
        // May be called again on every setup/activate, hence the guard.
        if (!service.isRunning()) {
            service.setThreadName("LB-Service");
            service.set<LoadBox, &LoadBox::runService>(this);
            service.startTimeout(100);
        }
    #endif
    }

    inline void process(uint32_t n_samples, float* output, float* output1) {
        engine.process(n_samples, output, output1);
    }

    void getLatency(uint32_t* latency) {
        (*latency) = static_cast<uint32_t>(engine.latency);
    }

    /****************************************************************
     ** IR/NAM file loading
     **
     ** engine.ir_file / ir_file1 are plain std::strings that the loader thread
     ** (engine.xrworker) both reads and, when a load fails, writes. Everyone who
     ** wants to load a file - the GUI, a host pushing state, a host writing a
     ** string property - therefore stages the request instead of writing them,
     ** and pumpPendingFiles() is the one place that moves a staged name into the
     ** engine, only ever while the loader is idle and one thread at a time.
     */

    // Request a file for a slot: 0 = left (ir_file), 1 = right (ir_file1).
    // "None" clears the slot. Returns false when the request is rejected
    // outright, so a caller can report a synchronous error rather than leave
    // the client waiting on a load that will never happen.
    bool setFileName(int slot, const std::string& fileName) {
        if (!stageFileName(slot, fileName)) return false;
        // Hand it over now if the loader happens to be free, rather than making
        // the caller wait for the next service tick. When it is busy the request
        // stays staged and the service thread picks it up.
        pumpPendingFiles();

        return true;
    }

    // Hand any staged file names to the loader thread, and report back what it
    // ended up loading. Returns true when work was handed over on this call.
    // Called from the service thread (headless) or the GUI timer thread.
    bool serviceEngine() {
        publishFileNames();
        return pumpPendingFiles();
    }

    // ParallelThread wants a void() member, hence the wrapper
    void runService() {
        serviceEngine();
    }

    /****************************************************************
     ** string properties (see PluginAPI.h)
     */

    int stringPropertyCount() const {
        return 2;
    }

    bool stringPropertyInfo(int index, StringPropertyInfo& info) const {
        switch (index) {
            case 0:
                info = { IrFileLeftPropertyId,  "ir_file_left",  "IR File Left",  false };
                return true;
            case 1:
                info = { IrFileRightPropertyId, "ir_file_right", "IR File Right", false };
                return true;
            default:
                return false;
        }
    }

    // Reports the last file name the loader thread confirmed, not the last one
    // requested - so a rejected or failed load reads back as "None", and a load
    // still in flight reads back as the previous value.
    bool getStringProperty(uint32_t id, std::string& value) const {
        const int slot = slotForPropertyId(id);
        if (slot < 0) return false;
        std::lock_guard<std::mutex> lock(pendingMutex);
        value = publishedFile[slot];
        return true;
    }

    bool setStringProperty(uint32_t id, const std::string& value) {
        const int slot = slotForPropertyId(id);
        if (slot < 0) return false;
        return setFileName(slot, value);
    }

    void setStringPropertyListener(IStringPropertyListener* listener) {
        propListener.store(listener, std::memory_order_release);
    }

#ifndef HEADLESS
    void getEngineValues() {
        adj_set_value(ui->widget[0]->adj, engine.IRoutputGain);
        adj_set_value(ui->widget[1]->adj, engine.IRoutputGain1);
        adj_set_value(ui->widget[2]->adj, static_cast<float>(engine.conv.get_normalisation()));
        adj_set_value(ui->widget[3]->adj, static_cast<float>(engine.conv1.get_normalisation()));
        adj_set_value(ui->widget[6]->adj, engine.IRmix);
        adj_set_value(ui->widget[7]->adj, engine.MasterOutGain);
        adj_set_value(ui->widget[8]->adj, static_cast<float>(engine.IRmode));
        adj_set_value(ui->widget[9]->adj, static_cast<float>(engine.bypass));
    }
#endif

    // send value changes from GUI to the engine
    void sendValueChanged(int port, float value) {
        switch (port) {
            // 0 + 1 audio ports (L/R in)
            case 7:
                engine.IRoutputGain = value;
                param.setParamDirty(1 , true);
            break;
            case 8:
                engine.IRoutputGain1 = value;
                param.setParamDirty(2 , true);
            break;
            case 9:
            {
                engine._cd.fetch_add(1, std::memory_order_relaxed);
                engine.conv.set_normalisation(static_cast<uint32_t>(value));
                if (engine.ir_file.compare("None") != 0) {
                    workToDo.store(true, std::memory_order_release);
                }
            }
            break;
            case 10:
            {
                engine._cd.fetch_add(2, std::memory_order_relaxed);
                engine.conv1.set_normalisation(static_cast<uint32_t>(value));
                if (engine.ir_file1.compare("None") != 0) {
                    workToDo.store(true, std::memory_order_release);
                }
            }
            break;
            case 14:
                engine.bypass = static_cast<uint32_t>(value);
                param.setParamDirty(0 , true);
            break;
            case 17:
                setFileName(0, "None");
            break;
            case 18:
                setFileName(1, "None");
            break;
            // 22 latency label, 23 xrun label: read-only, no case needed
            case 33:
                engine.IRmode = static_cast<uint32_t>(value);
                param.setParamDirty(3 , true);
            break;
            case 34:
                engine.IRmix = value;
                param.setParamDirty(4 , true);
            break;
            case 35:
                engine.MasterOutGain = value;
                param.setParamDirty(5 , true);
            break;
            default:
            break;
        }
        // inform the process thread that a controller value was changed by the GUI thread
        param.controllerChanged.store(true, std::memory_order_release);
    }

#ifndef HEADLESS
    // send a file name from GUI to the engine
    void sendFileName(ModelPicker* m) {
        switch(m->model) {
            case 3: setFileName(0, m->filename); break;
            case 4: setFileName(1, m->filename); break;
            default: break;
        }
    }
#endif

    float check_stod (const std::string& str) {
        char* point = localeconv()->decimal_point;
        if (std::string(".") != point) {
            std::string::size_type point_it = str.find(".");
            std::string temp_str = str;
            if (point_it != std::string::npos)
                temp_str.replace(point_it, point_it + 1, point);
            return std::stod(temp_str);
        } else return std::stod(str);
    }

    std::string remove_sub(std::string a, std::string b) {
        std::string::size_type fpos = a.find(b);
        if (fpos != std::string::npos )
            a.erase(a.begin() + fpos, a.begin() + fpos + b.length());
        return (a);
    }

    void readState(std::string _stream) {
        std::string stream = _stream;
        std::string line;
        std::string key;
        std::string value;
        std::size_t pos = _stream.find("|");
        while (pos != std::string::npos) {
            line = stream.substr(0, pos);
            std::istringstream buf(line);
            buf >> key;
            buf >> value;
            if (key.compare("[CONTROLS]") == 0) {
                engine.IRoutputGain = check_stod(value);
                buf >> value;
                engine.IRoutputGain1 = check_stod(value);
                buf >> value;
                engine.conv.set_normalisation((int)check_stod(value));
                buf >> value;
                engine.conv1.set_normalisation((int)check_stod(value));
                buf >> value;
                engine.bypass = static_cast<uint32_t>(check_stod(value));
                buf >> value;
                engine.IRmode = static_cast<uint32_t>(check_stod(value));
                buf >> value;
                engine.IRmix = check_stod(value);
                buf >> value;
                engine.MasterOutGain = check_stod(value);
            } else if (key.compare("[IrFile]") == 0) {
                // stage only: both slots are handed over together below
                stageFileName(0, remove_sub(line, "[IrFile] "));
            } else if (key.compare("[IrFile1]") == 0) {
                stageFileName(1, remove_sub(line, "[IrFile1] "));
            }
            key.clear();
            value.clear();
            stream = stream.substr(pos+1);
            pos = stream.find("|");
            if (pos == std::string::npos) break;
        }
        // apply straight away where possible, as a host restoring state expects
        // the plugin to come back up loaded. If the loader is busy the request
        // stays staged and the service thread retries.
        pumpPendingFiles();
    }

    void saveState(std::string *state) {
        std::ostringstream buffer;
        buffer << "[CONTROLS] ";
        buffer << engine.IRoutputGain << " ";
        buffer << engine.IRoutputGain1 << " ";
        buffer << engine.conv.get_normalisation() << " ";
        buffer << engine.conv1.get_normalisation() << " ";
        buffer << engine.bypass << " ";
        buffer << engine.IRmode << " ";
        buffer << engine.IRmix << " ";
        buffer << engine.MasterOutGain << " ";
        buffer << "|";
        // the confirmed names rather than engine.ir_file directly: this runs on
        // the host's thread, which is not the one that owns those strings
        std::string left, right;
        getStringProperty(IrFileLeftPropertyId, left);
        getStringProperty(IrFileRightPropertyId, right);
        buffer << "[IrFile] " << left << "|";
        buffer << "[IrFile1] " << right << "|";
        (*state) = buffer.str();
    }

#ifndef HEADLESS
    void cleanup() {
        plugin_cleanup(ui);
        free(ui->private_ptr);
        ui->private_ptr = NULL;
    }
#endif

private:
  #ifndef HEADLESS
    ParallelThread          fetch;
    X11_UI*                 ui;
    Window                  p;
    std::string             title;
    bool                    firstLoop;
  #endif
    irloader::Engine        engine;
    std::atomic<bool>       workToDo;

    ParallelThread          service;      // headless replacement for the GUI timer
    std::mutex              pumpMutex;    // serialises pumpPendingFiles()
    mutable std::mutex      pendingMutex; // guards all four arrays below
    std::string             pendingFile[2];
    bool                    pendingSet[2];
    std::string             publishedFile[2];
    std::atomic<IStringPropertyListener*> propListener;

    static bool endsWith(const std::string& str, const std::string& suffix) {
        if (str.size() < suffix.size()) return false;
        return str.compare(str.size() - suffix.size(), suffix.size(), suffix) == 0;
    }

    static int slotForPropertyId(uint32_t id) {
        switch (id) {
            case IrFileLeftPropertyId:  return 0;
            case IrFileRightPropertyId: return 1;
            default:                    return -1;
        }
    }

    static uint32_t propertyIdForSlot(int slot) {
        return slot == 0 ? IrFileLeftPropertyId : IrFileRightPropertyId;
    }

    // Validate and stage a request without touching the engine.
    bool stageFileName(int slot, const std::string& fileName) {
        if (slot < 0 || slot > 1) return false;
        if (fileName.compare("None") != 0) {
            if (!(endsWith(fileName, "nam") || endsWith(fileName, "wav") ||
                                               endsWith(fileName, "WAV")))
                return false;
            if (access(fileName.c_str(), R_OK) != 0) return false;
        }
        {
            std::lock_guard<std::mutex> lock(pendingMutex);
            pendingFile[slot] = fileName;
            pendingSet[slot] = true;
        }
        workToDo.store(true, std::memory_order_release);

        return true;
    }

    // Move staged file names into the engine and kick the loader thread.
    // The only writer of engine.ir_file / ir_file1 outside the loader itself.
    //
    // Callable from any non-realtime thread - a host setting a property, a host
    // restoring state, the GUI, the service thread - so pumpMutex keeps it to
    // one at a time. Between xrworker.getProcess() claiming the idle loader and
    // runProcess() releasing it, do_work_mono cannot be running, which is what
    // makes writing the engine's strings here safe.
    bool pumpPendingFiles() {
        std::lock_guard<std::mutex> pumping(pumpMutex);
        if (!workToDo.load(std::memory_order_acquire)) return false;
        if (!engine.xrworker.getProcess()) return false;   // loader busy, retry later

        int dirty = 0;
        {
            std::lock_guard<std::mutex> lock(pendingMutex);
            if (pendingSet[0]) {
                engine.ir_file = pendingFile[0];
                pendingSet[0] = false;
                dirty += 1;
            }
            if (pendingSet[1]) {
                engine.ir_file1 = pendingFile[1];
                pendingSet[1] = false;
                dirty += 2;
            }
        }
        if (dirty) engine._cd.fetch_add(dirty, std::memory_order_relaxed);
        workToDo.store(false, std::memory_order_release);
        engine.xrworker.runProcess();
        return true;
    }

    // Note what the loader actually ended up with, and tell the host. A failed
    // load leaves "None" behind (see Engine::setIRFile), which is exactly the
    // information a client needs, so this reports the engine's own strings
    // rather than what was requested.
    void publishFileNames() {
        // only safe to read the engine's strings while the loader is idle; it
        // rewrites them on failure
        if (!engine.xrworker.getState()) return;

        for (int slot = 0; slot < 2; slot++) {
            const std::string& live = (slot == 0) ? engine.ir_file : engine.ir_file1;
            std::string changed;
            {
                std::lock_guard<std::mutex> lock(pendingMutex);
                if (publishedFile[slot].compare(live) == 0) continue;
                publishedFile[slot] = live;
                changed = live;
            }
            // outside the lock: the listener calls into the host
            IStringPropertyListener* listener = propListener.load(std::memory_order_acquire);
            if (listener != nullptr)
                listener->stringPropertyChanged(propertyIdForSlot(slot), changed);
        }
    }

  #ifndef HEADLESS
    // rebuild file menu when needed
    void rebuild_file_menu(ModelPicker *m) {
        xevfunc store = m->fbutton->func.value_changed_callback;
        m->fbutton->func.value_changed_callback = dummy_callback;
        combobox_delete_entrys(m->fbutton);
        fp_get_files(m->filepicker, m->dir_name, 0, 1);
        int active_entry = m->filepicker->file_counter-1;
        for(uint32_t i = 0;i<m->filepicker->file_counter;i++) {
            combobox_add_entry(m->fbutton, m->filepicker->file_names[i]);
            if (strcmp(basename(m->filename),m->filepicker->file_names[i]) == 0)
                active_entry = i;
        }
        combobox_add_entry(m->fbutton, "None");
        adj_set_value(m->fbutton->adj, active_entry);
        combobox_set_menu_size(m->fbutton, min(14, m->filepicker->file_counter+1));
        m->fbutton->func.value_changed_callback = store;
    }

    // confirmation from engine that a file is loaded
    inline void get_file(std::string fileName, ModelPicker *m) {
        if (!fileName.empty() && (fileName.compare("None") != 0)) {
            const char* uri = fileName.c_str();
            if (strcmp(uri, (const char*)m->filename) !=0) {
                free(m->filename);
                m->filename = NULL;
                m->filename = strdup(uri);
                char *dn = strdup(dirname((char*)uri));
                if (m->dir_name == NULL || strcmp((const char*)m->dir_name,
                                                        (const char*)dn) !=0) {
                    free(m->dir_name);
                    m->dir_name = NULL;
                    m->dir_name = strdup(dn);
                    FileButton *filebutton = (FileButton*)m->filebutton->private_struct;
                    filebutton->path = m->dir_name;
                    rebuild_file_menu(m);
                }
                free(dn);
            }
        } else if (strcmp(m->filename, "None") != 0) {
            free(m->filename);
            m->filename = NULL;
            m->filename = strdup("None");
        }
    }
  #endif

};


/****************************************************************
 ** connect value change messages from the GUI (C) to the engine (C++)
 */

#ifndef HEADLESS
// send value changes from GUI to the engine
void sendValueChanged(X11_UI *ui, int port, float value) {
    LoadBox *r = (LoadBox*)ui->win->private_struct;
    r->sendValueChanged(port, value);
}

// send a file name from GUI to the engine
void sendFileName(X11_UI *ui, ModelPicker* m){
    LoadBox *r = (LoadBox*)ui->win->private_struct;
    r->sendFileName(m);
}
#endif
