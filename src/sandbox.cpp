#include "sandbox.h"

#include <sys/stat.h>   // stat
#include <algorithm>    // std::find
#include <fstream>
#include <math.h>
#include <memory>

#include "tools/job.h"
#include "tools/text.h"
#include "tools/record.h"
#include "tools/console.h"


#include "vera/ops/fs.h"
#include "vera/window.h"
#include "vera/ops/draw.h"
#include "vera/ops/math.h"
#include "vera/ops/meshes.h"
#include "vera/ops/string.h"
#include "vera/ops/pixel.h"
#include "vera/shaders/defaultShaders.h"
#include "vera/xr/holoPlay.h"

#include "glm/gtx/matrix_transform_2d.hpp"
#include "glm/gtx/rotate_vector.hpp"

#define TRACK_BEGIN(A) if (uniforms.tracker.isRunning()) uniforms.tracker.begin(A); 
#define TRACK_END(A) if (uniforms.tracker.isRunning()) uniforms.tracker.end(A); 

// ------------------------------------------------------------------------- CONTRUCTOR
Sandbox::Sandbox(): 
    frag_index(-1), vert_index(-1), geom_index(-1), 
    lenticular(""), quilt(-1), 
    verbose(false), cursor(true), fxaa(false),
    // Main Vert/Frag/Geom
    m_frag_source(""), m_vert_source(""),
    // Buffers
    m_buffers_total(0),
    // Poisson Fill
    m_pyramid_total(0),
    // PostProcessing
    m_postprocessing(false),
    // Geometry helpers
    m_cross_vbo(nullptr),
    // Plot helpers
    m_plot_texture(nullptr), m_plot(PLOT_OFF),

    // Record
    #if defined(SUPPORT_MULTITHREAD_RECORDING)
    m_task_count(0),
    /** allow 500 MB to be used for the image save queue **/
    m_max_mem_in_queue(500 * 1024 * 1024),
    m_save_threads(std::max(1, static_cast<int>(std::thread::hardware_concurrency()) - 1)),
    #endif

    // Scene
    m_view2d(1.0), m_time_offset(0.0), m_camera_elevation(1.0), m_camera_azimuth(180.0), m_frame(0), m_error_screen(vera::SHOW_MAGENTA_SHADER), m_change(true), m_initialized(false), 

    // Debug
    m_showTextures(false), m_showPasses(false)
{
    vera::setScene( (vera::Scene*)&uniforms );

    // TIME UNIFORMS
    //
    uniforms.functions["u_frame"] = UniformFunction( "int", [&](vera::Shader& _shader) {
        if (isRecording()) _shader.setUniform("u_frame", getRecordingFrame());
        _shader.setUniform("u_frame", (int)m_frame);
    }, 
    [&]() { 
        if (isRecording()) return vera::toString( getRecordingFrame() );
        else return vera::toString(m_frame, 1); 
    } );

    uniforms.functions["u_time"] = UniformFunction( "float", [&](vera::Shader& _shader) {
        if (isRecording()) _shader.setUniform("u_time", getRecordingTime());
        else _shader.setUniform("u_time", float(vera::getTime()) - m_time_offset);
    }, 
    [&]() {  
        if (isRecording()) return vera::toString( getRecordingTime() );
        else return vera::toString(vera::getTime() - m_time_offset); 
    } );

    uniforms.functions["u_delta"] = UniformFunction("float", [&](vera::Shader& _shader) {
        if (isRecording()) _shader.setUniform("u_delta", getRecordingDelta());
        else _shader.setUniform("u_delta", float(vera::getDelta()));
    }, 
    [&]() { 
        if (isRecording()) return vera::toString( getRecordingDelta() );
        else return vera::toString(vera::getDelta());
    });

    uniforms.functions["u_date"] = UniformFunction("vec4", [](vera::Shader& _shader) {
        _shader.setUniform("u_date", vera::getDate());
    },
    []() { return vera::toString(vera::getDate().x, 0) + "," + vera::toString(vera::getDate().y, 0) + "," + vera::toString(vera::getDate().z, 0) + "," + vera::toString(vera::getDate().w, 2); });

    // MOUSE
    uniforms.functions["u_mouse"] = UniformFunction("vec2", [](vera::Shader& _shader) {
        _shader.setUniform("u_mouse", float(vera::getMouseX()), float(vera::getMouseY()));
    },
    []() { return vera::toString(vera::getMouseX(),1) + "," + vera::toString(vera::getMouseY(),1); } );

    // VIEWPORT
    uniforms.functions["u_resolution"]= UniformFunction("vec2", [](vera::Shader& _shader) {
        _shader.setUniform("u_resolution", float(vera::getWindowWidth()), float(vera::getWindowHeight()));
    },
    []() { return vera::toString((float)vera::getWindowWidth(),1) + "," + vera::toString((float)vera::getWindowHeight(),1); });

    // SCENE
    uniforms.functions["u_scene"] = UniformFunction("sampler2D", [this](vera::Shader& _shader) {
        if (m_postprocessing && m_sceneRender_fbo.getTextureId()) {
            _shader.setUniformTexture("u_scene", &m_sceneRender_fbo, _shader.textureIndex++ );
        }
    });

    uniforms.functions["u_sceneDepth"] = UniformFunction("sampler2D", [this](vera::Shader& _shader) {
        if (m_postprocessing && m_sceneRender_fbo.getTextureId()) {
            _shader.setUniformDepthTexture("u_sceneDepth", &m_sceneRender_fbo, _shader.textureIndex++ );
        }
    });

    uniforms.functions["u_view2d"] = UniformFunction("mat3", [this](vera::Shader& _shader) {
        _shader.setUniform("u_view2d", m_view2d);
    });

    uniforms.functions["u_modelViewProjectionMatrix"] = UniformFunction("mat4");
}

Sandbox::~Sandbox() {
    #if defined(SUPPORT_MULTITHREAD_RECORDING)
    /** make sure every frame is saved before exiting **/
    if (m_task_count > 0)
        std::cout << "saving remaining frames to disk, this might take a while ..." << std::endl;
    
    while (m_task_count > 0)
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    
    #endif
}

// ------------------------------------------------------------------------- SET

void Sandbox::setup( WatchFileList &_files, CommandList &_commands ) {

    // Add Sandbox Commands
    // ----------------------------------------
    _commands.push_back(Command("debug", [&](const std::string& _line){
        if (_line == "debug") {
            std::string rta = m_showPasses ? "on" : "off";
            std::cout << "buffers," << rta << std::endl; 
            rta = m_showTextures ? "on" : "off";
            std::cout << "textures," << rta << std::endl;
            if (uniforms.models.size() > 0) {
                rta = m_sceneRender.showGrid ? "on" : "off";
                std::cout << "grid," << rta << std::endl; 
                rta = m_sceneRender.showAxis ? "on" : "off";
                std::cout << "axis," << rta << std::endl; 
                rta = m_sceneRender.showBBoxes ? "on" : "off";
                std::cout << "bboxes," << rta << std::endl;
            }
            return true;
        }
        else {
            std::vector<std::string> values = vera::split(_line,',');
            if (values.size() == 2) {
                m_showPasses = (values[1] == "on");
                m_showTextures = (values[1] == "on");
                console_uniforms( values[1] == "on" );
                // m_plot = (values[1] == "on")? 1 : 0;
                if (uniforms.models.size() > 0) {
                    m_sceneRender.showGrid = (values[1] == "on");
                    m_sceneRender.showAxis = (values[1] == "on");
                    m_sceneRender.showBBoxes = (values[1] == "on");
                    if (values[1] == "on") {
                        m_sceneRender.addDefine("DEBUG", values[1]);
                        uniforms.addDefine("DEBUG", values[1]);
                    }
                    else {
                        m_sceneRender.delDefine("DEBUG");
                        uniforms.delDefine("DEBUG");
                    }
                }
                // if (values[1] == "on") {
                //     m_plot = PLOT_FPS;
                //     m_plot_shader.addDefine("PLOT_VALUE", "color.rgb += digits(uv * 0.1 + vec2(0.105, -0.01), value.r * 60.0, 1.0);");
                // }
                // else {
                //     m_plot = PLOT_OFF;
                //     m_plot_shader.delDefine("PLOT_VALUE");
                // }
                return true;
            }
        }
        return false;
    },
    "debug[,on|off]", "show/hide debug elements or return the status of them", false));

    _commands.push_back(Command("track", [&](const std::string& _line){
        if (_line == "track") {
            std::cout << "track," << (uniforms.tracker.isRunning() ? "on" : "off") << std::endl; 
            return true;
        }
        else {
            std::vector<std::string> values = vera::split(_line,',');
            if (values.size() == 2) {

                if ((values[1] == "on" || values[1] == "start" | values[1] == "begin") &&
                    !uniforms.tracker.isRunning() ) 
                    uniforms.tracker.start();
                else if (   (values[1] == "off" || values[1] == "stop" | values[1] == "end") &&
                            uniforms.tracker.isRunning() ) 
                    uniforms.tracker.stop();
                    
                else if (values[1] == "average") 
                    std::cout << uniforms.tracker.logAverage();

                else if (values[1] == "samples")
                    std::cout << uniforms.tracker.logSamples();

                else if (values[1] == "framerate")
                    std::cout << uniforms.tracker.logFramerate();
            }

            else if (values.size() == 3) {

                if (values[1] == "average" && 
                    vera::haveExt(values[2],"csv") ) {
                    std::ofstream out(values[2]);
                    out << uniforms.tracker.logAverage();
                    out.close();
                }

                else if (values[1] == "average")
                    std::cout << uniforms.tracker.logAverage( values[2] );

                else if (   values[1] == "samples" && 
                            vera::haveExt(values[2],"csv") ) {
                    std::ofstream out(values[2]);
                    out << "track,timeStampMs,durationMs\n";
                    out << uniforms.tracker.logSamples();
                    out.close();
                }

                else if (values[1] == "samples")
                    std::cout << uniforms.tracker.logSamples(values[2]);
                    
            }
            else if (values.size() == 4) {

                if (values[1] == "average" && 
                    vera::haveExt(values[3],"csv") ) {
                    std::ofstream out( values[3] );
                    out << uniforms.tracker.logAverage( values[2] );
                    out.close();
                }

                else if (   values[1] == "samples" && 
                    vera::haveExt(values[3],"csv") ) {
                    std::ofstream out( values[3] );
                    out << uniforms.tracker.logSamples( values[2] );
                    out.close();
                }

            }

        }
        return false;
    },
    "track[,on|off|average|samples]", "start/stop tracking rendering time", false));

    _commands.push_back(Command("glsl_version", [&](const std::string& _line){ 
        if (_line == "glsl_version") {
            // Force the output in floats
            std::cout << vera::getVersion() << std::endl;
            return true;
        }
        return false;
    },
    "glsl_version", "return GLSL Version", false));

    _commands.push_back(Command("error_screen", [&](const std::string& _line){ 
        if (_line == "error_screen") {
            std::string rta = (m_error_screen == vera::SHOW_MAGENTA_SHADER) ? "on" : "off";
            std::cout << "error_screen," << rta << std::endl; 
            return true;
        }
        else {
            std::vector<std::string> values = vera::split(_line,',');
            if (values.size() == 2) {
                m_error_screen = (values[1] == "on")? vera::SHOW_MAGENTA_SHADER : vera::REVERT_TO_PREVIOUS_SHADER;
                return true;
            }
        }
        return false;
    },
    "error_screen,on|off", "enable/disable magenta screen on errors", false));

    _commands.push_back(Command("plot", [&](const std::string& _line){
        if (_line == "plot") {
            std::cout << "plot," << plot_options[m_plot] << std::endl; 
            return true;
        }
        else {
            std::vector<std::string> values = vera::split(_line,',');
            if (values[0] == "plot" && values.size() == 2) {

                m_plot_shader.delDefine("PLOT_VALUE");
                if (values[1] == "off") 
                    m_plot = PLOT_OFF;
                else if (values[1] == "luma") {
                    m_plot = PLOT_LUMA;
                     m_plot_shader.addDefine("PLOT_VALUE", "color.rgb = vec3(step(st.y, data.a)); color += stroke(fract(st.x * 5.0), 0.5, 0.025) * 0.1;");
                }
                else if (values[1] == "red") {
                    m_plot = PLOT_RED;
                     m_plot_shader.addDefine("PLOT_VALUE", "color.rgb = vec3(step(st.y, data.r), 0.0, 0.0);  color += stroke(fract(st.x * 5.0), 0.5, 0.025) * 0.1;");
                }
                else if (values[1] == "green") {
                    m_plot = PLOT_GREEN;
                    m_plot_shader.addDefine("PLOT_VALUE", "color.rgb = vec3(0.0, step(st.y, data.g), 0.0);  color += stroke(fract(st.x * 5.0), 0.5, 0.025) * 0.1;");
                }
                else if (values[1] == "blue") {
                    m_plot = PLOT_BLUE;
                    m_plot_shader.addDefine("PLOT_VALUE", "color.rgb = vec3(0.0, 0.0, step(st.y, data.b));  color += stroke(fract(st.x * 5.0), 0.5, 0.025) * 0.1;");
                }
                else if (values[1] == "rgb") {
                    m_plot = PLOT_RGB;
                    m_plot_shader.addDefine("PLOT_VALUE", "color += stroke(fract(st.x * 5.0), 0.5, 0.025) * 0.1;");
                }
                else if (values[1] == "fps") {
                    m_plot = PLOT_FPS;
                    m_plot_shader.addDefine("PLOT_VALUE", "color.rgb += digits(uv * 0.1 + vec2(0.0, -0.01), value.r * 60.0, 1.0); color += stroke(fract(st.y * 3.0), 0.5, 0.05) * 0.1;");
                }
                else if (values[1] == "ms") {
                    m_plot = PLOT_MS;
                    m_plot_shader.addDefine("PLOT_VALUE", "color.rgb += digits(uv * 0.1 + vec2(0.105, -0.01), value.r * 60.0, 1.0); color += stroke(fract(st.y * 3.0), 0.5, 0.05) * 0.1;");
                }
                return true;
            }
        }
        return false;
    },
    "plot[,off|luma|red|green|blue|rgb|fps|ms]", "show/hide a histogram or FPS plot on screen", false));

    _commands.push_back(Command("reset", [&](const std::string& _line){
        if (_line == "reset") {
            m_time_offset = vera::getTime();
            return true;
        }
        return false;
    },
    "reset", "reset timestamp back to zero", false));

    _commands.push_back(Command("time", [&](const std::string& _line){ 
        if (_line == "time") {
            // Force the output in floats
            std::cout << std::setprecision(6) << (vera::getTime() - m_time_offset) << std::endl;
            return true;
        }
        return false;
    },
    "time", "return u_time, the elapsed time.", false));

    _commands.push_back(Command("defines", [&](const std::string& _line){ 
        if (_line == "defines") {
            if (uniforms.models.size() > 0) {
                m_sceneRender.printDefines();
                uniforms.printDefines();
            }
            else
                m_canvas_shader.printDefines();
            
            return true;
        }
        return false;
    },
    "defines", "return a list of active defines", false));
    
    _commands.push_back(Command("uniforms", [&](const std::string& _line){ 
        std::vector<std::string> values = vera::split(_line,',');

        if (values[0] != "uniforms")
            return false;

        if (values.size() == 1) {
            uniforms.printAvailableUniforms(false);
            uniforms.printDefinedUniforms();
            return true;
        }
        else if (values[1] == "all") {
            uniforms.printAvailableUniforms(true);
            uniforms.printDefinedUniforms();
            uniforms.printBuffers();
            uniforms.printTextures();
            uniforms.printStreams();
            uniforms.printCubemaps();
            uniforms.printLights();
            uniforms.printCameras();
            return true;
        }
        else if (values[1] == "active") {
            uniforms.printAvailableUniforms(false);
            uniforms.printDefinedUniforms();
            return true;
        }
        else if (values[1] == "defined") {
            uniforms.printDefinedUniforms(true);
            return true;
        }
        else if (values[1] == "textures") {
            uniforms.printTextures();
            uniforms.printBuffers();
            uniforms.printStreams();
            return true;
        }
        else if (values[1] == "buffers") {
            uniforms.printBuffers();
            return true;
        }
        else if (values[1] == "streams") {
            uniforms.printStreams();
            return true;
        }
        else if (values[1] == "cubemaps") {
            uniforms.printCubemaps();
            return true;
        }
        else if (values[1] == "cameras") {
            uniforms.printCameras();
            return true;
        }
        else if (values[1] == "lights") {
            uniforms.printLights();
            return true;
        }
        else if (values[1] == "on" || values[1] == "off") { 
            console_uniforms( values[1] == "on" );
            return true; 
        }

        return false;
    },
    "uniforms[,all|active|defined|textures|buffers|cubemaps|lights|cameras|on|off]", "return a list of uniforms", false));

    _commands.push_back(Command("textures", [&](const std::string& _line){ 
        if (_line == "textures") {
            uniforms.printTextures();
            uniforms.printStreams();
            return true;
        }
        else {
            std::vector<std::string> values = vera::split(_line,',');
            if (values.size() == 2) {
                m_showTextures = (values[1] == "on");
                return true;
            }
        }
        return false;
    },
    "textures[,on|off]", "return a list of textures as their uniform name and path. Or show/hide textures on viewport.", false));

    _commands.push_back(Command("buffers", [&](const std::string& _line){ 
        if (_line == "buffers") {
            uniforms.printBuffers();
            if (m_postprocessing) {
                if (lenticular.size() > 0)
                    std::cout << "LENTICULAR";
                else if (fxaa)
                    std::cout << "FXAA";
                else
                    std::cout << "Custom";
                std::cout << " postProcessing pass" << std::endl;
                return true;
            }
            
            return false;
        }
        else {
            std::vector<std::string> values = vera::split(_line,',');
            if (values.size() == 2) {
                m_showPasses = (values[1] == "on");
                return true;
            }
        }
        return false;
    },
    "buffers[,on|off]", "return a list of buffers as their uniform name. Or show/hide buffer on viewport.", false));

    // CUBEMAPS
    _commands.push_back(Command("cubemaps", [&](const std::string& _line){
        if (_line == "cubemaps") {
            uniforms.printCubemaps();
            return true;
        }
        return false;
    },
    "cubemaps", "print all cubemaps"));

    _commands.push_back(Command("sun_elevation", [&](const std::string& _line){ 
        std::vector<std::string> values = vera::split(_line,',');
        if (values.size() == 2) {

            float elevation = glm::radians( vera::toFloat(values[1]) );
            float azimuth = uniforms.getSunAzimuth();

            uniforms.addDefine("SUN", "u_light");
            uniforms.setSunPosition(azimuth, elevation, glm::length( uniforms.lights["default"]->getPosition() ));
            uniforms.activeCubemap = uniforms.cubemaps["default"];
            return true;
        }
        else {
            std::cout << uniforms.getSunElevation() << std::endl;
            return true;
        }
        return false;
    },
    "sun_elevation[,<degrees>]", "get or set the sun elevation in degrees (remember to skybox,on)."));

    _commands.push_back(Command("sun_azimuth", [&](const std::string& _line){ 
        std::vector<std::string> values = vera::split(_line,',');
        if (values.size() == 2) {

            float elevation = uniforms.getSunElevation();
            float azimuth = glm::radians( vera::toFloat(values[1]) );

            uniforms.addDefine("SUN", "u_light");
            uniforms.setSunPosition(azimuth, elevation, glm::length( uniforms.lights["default"]->getPosition() ) );
            uniforms.activeCubemap = uniforms.cubemaps["default"];

            return true;
        }
        else {
            std::cout << uniforms.getSunAzimuth() << std::endl;
            return true;
        }
        return false;
    },
    "sun_azimuth[,<degrees>]", "get or set the sun azimuth in degrees (remember to skybox,on)."));

    _commands.push_back(Command("sky_turbidity", [&](const std::string& _line){ 
        std::vector<std::string> values = vera::split(_line,',');
        if (values.size() == 2) {
            uniforms.setSkyTurbidity( vera::toFloat(values[1]) );
            uniforms.activeCubemap = uniforms.cubemaps["default"];
            return true;
        }
        else {
            std::cout << uniforms.getSkyTurbidity() << std::endl;
            return true;
        }
        return false;
    },
    "sky_turbidity[,<sky_turbidty>]", "get or set the sky turbidity."));

    // LIGTH
    _commands.push_back(Command("lights", [&](const std::string& _line){ 
        if (_line == "lights") {
            uniforms.printLights();
            return true;
        }
        return false;
    },
    "lights", "print all light related uniforms"));

    _commands.push_back(Command("light_position", [&](const std::string& _line){ 
        
        std::vector<std::string> values = vera::split(_line,',');
        if (values.size() == 4) {
            if (uniforms.lights.size() == 1)
                uniforms.setSunPosition( glm::vec3(vera::toFloat(values[1]), vera::toFloat(values[2]), vera::toFloat(values[3])) );
            
            return true;
        }
        // else if (values.size() == 5) {
        //     size_t i = vera::toInt(values[1]);
        //     if (uniforms.lights.size() > i)
        //         uniforms.lights[i].setPosition(glm::vec3(vera::toFloat(values[2]), vera::toFloat(values[3]), vera::toFloat(values[4])));

        //     return true;
        // }
        else {
            glm::vec3 pos = uniforms.lights["default"]->getPosition();
            std::cout << pos.x << ',' << pos.y << ',' << pos.z << std::endl;
            return true;
        }
        return false;
    },
    "light_position[[,<index>],<x>,<y>,<z>]", "get or set the light position"));

    _commands.push_back(Command("light_color", [&](const std::string& _line){ 
        std::vector<std::string> values = vera::split(_line,',');
        if (values.size() == 4) {
            vera::Light* sun = uniforms.lights["default"];
            sun->color = glm::vec3(vera::toFloat(values[1]), vera::toFloat(values[2]), vera::toFloat(values[3]));
            sun->bChange = true;
            return true;
        }
        // else if (values.size() == 5) {
        //     size_t i = vera::toInt(values[1]);
        //     if (uniforms.lights.size() > i) {
        //         uniforms.lights[i].color = glm::vec3(vera::toFloat(values[2]), vera::toFloat(values[3]), vera::toFloat(values[4]));
        //         uniforms.lights[i].bChange = true;
        //     }
        //     return true;
        // }
        else {
            glm::vec3 color = uniforms.lights["default"]->color;
            std::cout << color.x << ',' << color.y << ',' << color.z << std::endl;
            return true;
        }
        return false;
    },
    "light_color[,<r>,<g>,<b>]", "get or set the light color"));

    _commands.push_back(Command("light_falloff", [&](const std::string& _line){ 
         std::vector<std::string> values = vera::split(_line,',');
        if (values.size() == 2) {
            vera::Light* sun = uniforms.lights["default"];
            sun->falloff = vera::toFloat(values[1]);
            sun->bChange = true;
            return true;
        }
        // else if (values.size() == 5) {
        //     size_t i = vera::toInt(values[1]);
        //     if (uniforms.lights.size() > i) {
        //         uniforms.lights[i].falloff = vera::toFloat(values[2]);
        //         uniforms.lights[i].bChange = true;
        //     }
        //     return true;
        // }
        else {
            std::cout << uniforms.lights["default"]->falloff << std::endl;
            return true;
        }
        return false;
    },
    "light_falloff[,<value>]", "get or set the light falloff distance"));

    _commands.push_back(Command("light_intensity", [&](const std::string& _line){ 
         std::vector<std::string> values = vera::split(_line,',');
        if (values.size() == 2) {
            vera::Light* sun = uniforms.lights["default"];
            sun->intensity = vera::toFloat(values[1]);
            sun->bChange = true;
            return true;
        }
        // else if (values.size() == 5) {
        //     size_t i = vera::toInt(values[1]);
        //     if (uniforms.lights.size() > i) {
        //         uniforms.lights[i].intensity = vera::toFloat(values[2]);
        //         uniforms.lights[i].bChange = true;
        //     }
        //     return true;
        // }
        else {
            std::cout <<  uniforms.lights["default"]->intensity << std::endl;
            return true;
        }
        return false;
    },
    "light_intensity[,<value>]", "get or set the light intensity"));

    // CAMERA
    _commands.push_back(Command("camera_distance", [&](const std::string& _line){ 
        if (!uniforms.activeCamera) 
            return false;

        std::vector<std::string> values = vera::split(_line,',');
        if (values.size() == 2) {
            uniforms.activeCamera->setDistance(vera::toFloat(values[1]));
            return true;
        }
        else {
            
            std::cout << uniforms.activeCamera->getDistance() << std::endl;
            return true;
        }
        return false;
    },
    "camera_distance[,<dist>]", "get or set the camera distance to the target"));

    _commands.push_back(Command("camera_type", [&](const std::string& _line){ 
        if (!uniforms.activeCamera) 
            return false;

        std::vector<std::string> values = vera::split(_line,',');
        if (values.size() == 2) {
            if (values[1] == "ortho")
                uniforms.activeCamera->setProjection(vera::ProjectionType::ORTHO);
            else if (values[1] == "perspective")
                uniforms.activeCamera->setProjection(vera::ProjectionType::PERSPECTIVE);
            return true;
        }
        else {
            vera::ProjectionType type = uniforms.activeCamera->getProjectionType();
            if (type == vera::ProjectionType::ORTHO)
                std::cout << "ortho" << std::endl;
            else
                std::cout << "perspective" << std::endl;
            
            return true;
        }
        return false;
    },
    "camera_type[,<ortho|perspective>]", "get or set the camera type"));

    _commands.push_back(Command("camera_fov", [&](const std::string& _line){ 
        if (!uniforms.activeCamera) 
            return false;

        std::vector<std::string> values = vera::split(_line,',');
        if (values.size() == 2) {
            uniforms.activeCamera->setFOV( vera::toFloat(values[1]) );
            return true;
        }
        else {
            std::cout << uniforms.activeCamera->getFOV() << std::endl;
            return true;
        }
        return false;
    },
    "camera_fov[,<field_of_view>]", "get or set the camera field of view."));

    _commands.push_back(Command("camera_position", [&](const std::string& _line) {
        if (!uniforms.activeCamera) 
            return false;

        std::vector<std::string> values = vera::split(_line,',');
        if (values.size() == 4) {
            uniforms.activeCamera->setPosition( -glm::vec3(vera::toFloat(values[1]), vera::toFloat(values[2]), vera::toFloat(values[3])));
            uniforms.activeCamera->lookAt( uniforms.activeCamera->getTarget() );
            glm::vec3 v = uniforms.activeCamera->getPosition();
            m_camera_azimuth = glm::degrees( atan2(v.x, v.z) );
            m_camera_elevation = glm::degrees( atan2(-v.y, sqrt(v.x * v.x + v.z * v.z)) );
            return true;
        }
        else {
            glm::vec3 pos = -uniforms.activeCamera->getPosition();
            std::cout << pos.x << ',' << pos.y << ',' << pos.z << std::endl;
            return true;
        }
        return false;
    },
    "camera_position[,<x>,<y>,<z>]", "get or set the camera position."));

    _commands.push_back(Command("camera_exposure", [&](const std::string& _line) { 
        if (!uniforms.activeCamera) 
            return false;

        std::vector<std::string> values = vera::split(_line,',');
        if (values.size() == 4) {
            uniforms.activeCamera->setExposure( vera::toFloat(values[1]), vera::toFloat(values[2]), vera::toFloat(values[3]));
            return true;
        }
        else {
            std::cout << uniforms.activeCamera->getExposure() << std::endl;
            return true;
        }
        return false;
    },
    "camera_exposure[,<aper.>,<shutter>,<sensit.>]", "get or set the camera exposure values."));

     _commands.push_back(Command("stream", [&](const std::string& _line){ 
        std::vector<std::string> values = vera::split(_line,',');

        if (values.size() == 3) {
            if ( values[2] == "play") {
                uniforms.setStreamPlay( values[1] );
                return true;
            }
            else if ( values[2] == "stop") {
                uniforms.setStreamStop( values[1] );
                return true;
            }
            else if ( values[2] == "restart") {
                uniforms.setStreamRestart( values[1] );
                return true;
            }
            else if ( values[2] == "speed") {
                std::cout << uniforms.getStreamSpeed( values[1] ) << std::endl;
                return true;
            }
            else if ( values[2] == "time") {
                std::cout << uniforms.getStreamTime( values[1] ) << std::endl;
                return true;
            }
            else if ( values[2] == "pct") {
                std::cout << uniforms.getStreamPct( values[1] ) << std::endl;
                return true;
            }
        }
        else if (values.size() == 4) {
            if ( values[2] == "speed") {
                uniforms.setStreamSpeed( values[1], vera::toFloat(values[3]) );
                return true;
            }
            else if ( values[2] == "time") {
                uniforms.setStreamTime( values[1], vera::toFloat(values[3]) );
                return true;
            }
            else if ( values[2] == "pct") {
                uniforms.setStreamPct( values[1], vera::toFloat(values[3]) );
                return true;
            }
        }

        return false;
    },
    "stream,<uniform_name>,stop|play|speed|time[,<value>]", "play/stop or change speed or time of a specific stream"));

    _commands.push_back(Command("streams", [&](const std::string& _line){ 
        std::vector<std::string> values = vera::split(_line,',');

        if (_line == "streams")
            uniforms.printStreams();

        else if (values.size() == 2) {
            if ( values[1] == "stop") {
                uniforms.setStreamsStop();
                return true;
            }
            else if ( values[1] == "play") {
                uniforms.setStreamsPlay();
                return true;
            }
            else if ( values[1] == "restart") {
                uniforms.setStreamsRestart();
                return true;
            }
        }
        else if (values.size() == 3) {
            if ( values[1] == "speed") {
                uniforms.setStreamsSpeed( vera::toFloat(values[2]) );
                return true;
            }
            else if ( values[1] == "time") {
                uniforms.setStreamsTime( vera::toFloat(values[2]) );
                return true;
            }
            else if ( values[1] == "pct") {
                uniforms.setStreamsPct( vera::toFloat(values[2]) );
                return true;
            }
            else if ( values[1] == "prevs") {
                int prevs = vera::toInt(values[2]);

                if (prevs == 0) {
                    uniforms.setStreamsPrevs( 0 );
                    delDefine("STREAMS_PREVS");
                }
                else {
                    uniforms.setStreamsPrevs( vera::toInt(values[2]) );
                    addDefine("STREAMS_PREVS", values[2]);
                }
                return true;
            }
        }

        return false;
    },
    "streams[,stop|play|restart|speed|prevs[,<value>]]", "print all streams or get/set streams speed and previous frames"));

    #if defined(SUPPORT_MULTITHREAD_RECORDING)
    _commands.push_back(Command("max_mem_in_queue", [&](const std::string & line) {
        std::vector<std::string> values = vera::split(line,',');
        if (values.size() == 2) {
            m_max_mem_in_queue = std::stoll(values[1]);
        }
        else {
            std::cout << m_max_mem_in_queue.load() << std::endl;
        }
        return false;
    }, "max_mem_in_queue[,<bytes>]", "set the maximum amount of memory used by a queue to export images to disk"));
    #endif

    _commands.push_back(Command("pcl_plane", [&](const std::string & line) {
        std::vector<std::string> values = vera::split(line,',');
        int size_i = 512;

        if (values.size() > 1)
            size_i  = vera::toInt(values[1]);

        float size_f = size_i;
        vera::Mesh pcl;
        pcl.setDrawMode(vera::POINTS);
        for (int y = 0; y < size_i; y++) 
            for (int x = 0; x < size_i; x++)
                pcl.addVertex(glm::vec3(x/size_f, y/size_f, 0.0f));
            
        if (uniforms.models.size() == 0)
            m_sceneRender.setup( _commands, uniforms);

        // Add Model with pcl mesh
        uniforms.models["pcl_plane"] = new vera::Model("pcl_plane", pcl);
        m_sceneRender.loadScene(uniforms);
        uniforms.activeCamera->orbit(m_camera_azimuth, m_camera_elevation, m_sceneRender.getArea() * 2.0);
        uniforms.activeCamera->lookAt(uniforms.activeCamera->getTarget());

        #ifdef __EMSCRIPTEN__
        // Commands are parse in the main GL loop in EMSCRIPTEN
        m_sceneRender.loadShaders(uniforms, m_frag_source, m_vert_source, false);

        #else
        // Commands are intepreted on a different thread on non-EMSCRIPTEN 
        //  so need to trigger a reload by changing the timestamps of files
        for (size_t i = 0; i < _files.size(); i++)
            _files[i].lastChange = 0;
        
        #endif

        return true;
    }, "pcl_plane[,<RESOLUTION>]", "add a pointcloud plane"));

    // LOAD SHACER 
    // -----------------------------------------------
    if (frag_index != -1) {
        // If there is a Fragment shader load it
        m_frag_source = "";
        m_frag_dependencies.clear();

        if ( !vera::loadGlslFrom(_files[frag_index].path, &m_frag_source, include_folders, &m_frag_dependencies) )
            return;

        vera::setVersionFromCode(m_frag_source);
    }
    else {
        // If there is no use the default one
        if (geom_index == -1)
            m_frag_source = vera::getDefaultSrc(vera::FRAG_DEFAULT);
        else
            m_frag_source = vera::getDefaultSrc(vera::FRAG_DEFAULT_SCENE);
    }

    if (vert_index != -1) {
        // If there is a Vertex shader load it
        m_vert_source = "";
        m_vert_dependencies.clear();

        vera::loadGlslFrom(_files[vert_index].path, &m_vert_source, include_folders, &m_vert_dependencies);
    }
    else {
        // If there is no use the default one
        if (geom_index == -1)
            m_vert_source = vera::getDefaultSrc(vera::VERT_DEFAULT);
        else
            m_vert_source = vera::getDefaultSrc(vera::VERT_DEFAULT_SCENE);
    }

    if (vert_index != -1 || geom_index != -1)
        m_sceneRender.setup( _commands, uniforms);

    // LOAD GEOMETRY
    // -----------------------------------------------
    if (geom_index != -1) {
        uniforms.load(_files[geom_index].path, verbose);
        m_sceneRender.loadScene(uniforms);
        uniforms.activeCamera->orbit(m_camera_azimuth, m_camera_elevation, m_sceneRender.getArea() * 2.0);
    }
    else {
        m_canvas_shader.addDefine("MODEL_VERTEX_TEXCOORD", "v_texcoord");
        uniforms.activeCamera->orbit(m_camera_azimuth, m_camera_elevation, 2.0);
    }

    uniforms.activeCamera->lookAt( uniforms.activeCamera->getTarget() );

    // FINISH SCENE SETUP
    // -------------------------------------------------
    uniforms.activeCamera->setViewport(vera::getWindowWidth(), vera::getWindowHeight());

    if (lenticular.size() > 0)
        vera::setLenticularProperties(lenticular);

    if (quilt >= 0) {
        vera::setQuiltProperties(quilt);
        addDefine("QUILT", vera::toString(quilt));
        addDefine("QUILT_WIDTH", vera::toString( vera::getQuiltWidth() ));
        addDefine("QUILT_HEIGHT", vera::toString( vera::getQuiltHeight() ));
        addDefine("QUILT_COLUMNS", vera::toString( vera::getQuiltColumns() ));
        addDefine("QUILT_ROWS", vera::toString( vera::getQuiltRows() ));
        addDefine("QUILT_TOTALVIEWS", vera::toString( vera::getQuiltTotalViews() ));

        uniforms.activeCamera->setFOV(glm::radians(14.0f));
        uniforms.activeCamera->setProjection(vera::ProjectionType::PERSPECTIVE_VIRTUAL_OFFSET);
        // uniforms.activeCamera->setClipping(0.01, 100.0);

        if (geom_index != -1)
            uniforms.activeCamera->orbit(m_camera_elevation, m_camera_azimuth, m_sceneRender.getArea() * 8.5);

        if (lenticular.size() == 0)
            vera::setWindowSize(vera::getQuiltWidth(), vera::getQuiltHeight());
    }

    // Prepare viewport
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    glDisable(GL_DEPTH_TEST);
    glFrontFace(GL_CCW);

    // Turn on Alpha blending
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_DST_ALPHA);

    // Clear the background
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    // LOAD SHADERS
    reloadShaders( _files );

    // TODO:
    //      - this seams to solve the problem of buffers not properly initialize
    //      - digg deeper
    //
    uniforms.buffers.clear();
    uniforms.doubleBuffers.clear();
    _updateBuffers();

    flagChange();
}

void Sandbox::addDefine(const std::string &_define, const std::string &_value) {
    for (int i = 0; i < m_buffers_total; i++)
        m_buffers_shaders[i].addDefine(_define, _value);

    for (int i = 0; i < m_doubleBuffers_total; i++)
        m_doubleBuffers_shaders[i].addDefine(_define, _value);

    if (uniforms.models.size() > 0)
        m_sceneRender.addDefine(_define, _value);
    else
        m_canvas_shader.addDefine(_define, _value);

    m_postprocessing_shader.addDefine(_define, _value);
}

void Sandbox::delDefine(const std::string &_define) {
    for (int i = 0; i < m_buffers_total; i++)
        m_buffers_shaders[i].delDefine(_define);

    for (int i = 0; i < m_doubleBuffers_total; i++)
        m_doubleBuffers_shaders[i].delDefine(_define);

    if (uniforms.models.size() > 0)
        m_sceneRender.delDefine(_define);
    else
        m_canvas_shader.delDefine(_define);

    m_postprocessing_shader.delDefine(_define);
}

// ------------------------------------------------------------------------- GET

bool Sandbox::isReady() {
    return m_initialized;
}

void Sandbox::flagChange() { 
    m_change = true;
}

void Sandbox::unflagChange() {
    m_change = false;
    m_sceneRender.unflagChange();
    uniforms.unflagChange();
}

bool Sandbox::haveChange() { 
    return  m_change ||
            isRecording() ||
            screenshotFile != "" ||
            m_sceneRender.haveChange() ||
            uniforms.haveChange();
}

const std::string& Sandbox::getSource(ShaderType _type) const {
    return (_type == FRAGMENT)? m_frag_source : m_vert_source;
}

// ------------------------------------------------------------------------- RELOAD SHADER

void Sandbox::_updateSceneBuffer(int _width, int _height) {
    vera::FboType type = uniforms.functions["u_sceneDepth"].present ? vera::COLOR_DEPTH_TEXTURES : vera::COLOR_TEXTURE_DEPTH_BUFFER;

    if (quilt >= 0) {
        _width = vera::getQuiltWidth();
        _height= vera::getQuiltHeight();
    }

    if (!m_sceneRender_fbo.isAllocated() ||
        m_sceneRender_fbo.getType() != type || 
        m_sceneRender_fbo.getWidth() != _width || 
        m_sceneRender_fbo.getHeight() != _height )
        m_sceneRender_fbo.allocate(_width, _height, type);
}

bool Sandbox::setSource(ShaderType _type, const std::string& _source) {
    if (_type == FRAGMENT) m_frag_source = _source;
    else  m_vert_source = _source;

    return true;
};

bool Sandbox::reloadShaders( WatchFileList &_files ) {
    flagChange();

    // UPDATE scene shaders of models (materials)
    if (uniforms.models.size() > 0) {
        if (verbose)
            std::cout << "Reload 3D scene shaders" << std::endl;

        m_sceneRender.loadShaders(uniforms, m_frag_source, m_vert_source, verbose);
    }
    else {
        if (verbose)
            std::cout << "Reload 2D shaders" << std::endl;

        // Reload the shader
        m_canvas_shader.detach(GL_FRAGMENT_SHADER | GL_VERTEX_SHADER);
        m_canvas_shader.load(m_frag_source, m_vert_source, m_error_screen, verbose);
    }

    // UPDATE shaders dependencies
    {
        vera::StringList new_dependencies = vera::merge(m_frag_dependencies, m_vert_dependencies);

        // remove old dependencies
        for (int i = _files.size() - 1; i >= 0; i--)
            if (_files[i].type == GLSL_DEPENDENCY)
                _files.erase( _files.begin() + i);

        // Add new dependencies
        struct stat st;
        for (size_t i = 0; i < new_dependencies.size(); i++) {
            WatchFile file;
            file.type = GLSL_DEPENDENCY;
            file.path = new_dependencies[i];
            stat( file.path.c_str(), &st );
            file.lastChange = st.st_mtime;
            _files.push_back(file);

            if (verbose)
                std::cout << " Watching file " << new_dependencies[i] << " as a dependency " << std::endl;
        }
    }

    // UPDATE uniforms
    uniforms.checkUniforms(m_vert_source, m_frag_source); // Check active native uniforms
    uniforms.flagChange();                                  // Flag all user defined uniforms as changed

    if ( uniforms.activeCubemap != nullptr ) {
        addDefine("SCENE_SH_ARRAY", "u_SH");
        addDefine("SCENE_CUBEMAP", "u_cubeMap");
    }

    // UPDATE Buffers
    m_buffers_total = countBuffers(m_frag_source);
    m_doubleBuffers_total = countDoubleBuffers(m_frag_source);
    m_pyramid_total = countConvolutionPyramid( getSource(FRAGMENT) );
    _updateBuffers();
    
    // UPDATE Postprocessing
    bool havePostprocessing = checkPostprocessing( getSource(FRAGMENT) );
    if (havePostprocessing) {
        // Specific defines for this buffer
        m_postprocessing_shader.addDefine("POSTPROCESSING");
        m_postprocessing_shader.load(m_frag_source, vera::getDefaultSrc(vera::VERT_BILLBOARD), vera::SHOW_MAGENTA_SHADER, false);
        m_postprocessing = havePostprocessing;
    }
    else if (lenticular.size() > 0) {
        m_postprocessing_shader.load(vera::getLenticularFragShader(vera::getVersion()), vera::getDefaultSrc(vera::VERT_BILLBOARD), vera::SHOW_MAGENTA_SHADER, false);
        uniforms.functions["u_scene"].present = true;
        m_postprocessing = true;
    }
    else if (fxaa) {
        m_postprocessing_shader.load(vera::getDefaultSrc(vera::FRAG_FXAA), vera::getDefaultSrc(vera::VERT_BILLBOARD), vera::SHOW_MAGENTA_SHADER, false);
        uniforms.functions["u_scene"].present = true;
        m_postprocessing = true;
    }
    else 
        m_postprocessing = false;

    if (m_postprocessing || m_plot == PLOT_RGB || m_plot == PLOT_RED || m_plot == PLOT_GREEN || m_plot == PLOT_BLUE || m_plot == PLOT_LUMA)
        _updateSceneBuffer(vera::getWindowWidth(), vera::getWindowHeight());

    console_refresh();

    return true;
}

// ------------------------------------------------------------------------- UPDATE
void Sandbox::_updateBuffers() {
    if ( m_buffers_total != int(uniforms.buffers.size()) ) {

        if (verbose)
            std::cout << "Creating/Removing " << uniforms.buffers.size() << " buffers to " << m_buffers_total << std::endl;

        uniforms.buffers.clear();
        m_buffers_shaders.clear();

        for (int i = 0; i < m_buffers_total; i++) {
            // New FBO
            uniforms.buffers.push_back( vera::Fbo() );

            glm::vec2 size = glm::vec2(vera::getWindowWidth(), vera::getWindowHeight());
            uniforms.buffers[i].fixed = getBufferSize(m_frag_source, "u_buffer" + vera::toString(i), size);
            uniforms.buffers[i].allocate(size.x, size.y, vera::COLOR_FLOAT_TEXTURE);
            
            // New Shader
            m_buffers_shaders.push_back( vera::Shader() );
            m_buffers_shaders[i].addDefine("BUFFER_" + vera::toString(i));
            m_buffers_shaders[i].load(m_frag_source, vera::getDefaultSrc(vera::VERT_BILLBOARD), vera::SHOW_MAGENTA_SHADER, false);
        }
    }
    else {
        for (size_t i = 0; i < m_buffers_shaders.size(); i++) {

            // Reload shader code
            m_buffers_shaders[i].addDefine("BUFFER_" + vera::toString(i));
            m_buffers_shaders[i].load(m_frag_source, vera::getDefaultSrc(vera::VERT_BILLBOARD), vera::SHOW_MAGENTA_SHADER, false);
        }
    }

    if ( m_doubleBuffers_total != int(uniforms.doubleBuffers.size()) ) {

        if (verbose)
            std::cout << "Creating/Removing " << uniforms.doubleBuffers.size() << " double buffers to " << m_doubleBuffers_total << std::endl;

        uniforms.doubleBuffers.clear();
        m_doubleBuffers_shaders.clear();

        for (int i = 0; i < m_doubleBuffers_total; i++) {
            // New FBO
            uniforms.doubleBuffers.push_back( vera::PingPong() );

            glm::vec2 size = glm::vec2(vera::getWindowWidth(), vera::getWindowHeight());
            bool fixed = getBufferSize(m_frag_source, "u_doubleBuffer" + vera::toString(i), size);
            uniforms.doubleBuffers[i][0].fixed = fixed;
            uniforms.doubleBuffers[i][1].fixed = fixed;
            uniforms.doubleBuffers[i].allocate(size.x, size.y, vera::COLOR_FLOAT_TEXTURE);
            
            // New Shader
            m_doubleBuffers_shaders.push_back( vera::Shader() );
            m_doubleBuffers_shaders[i].addDefine("DOUBLE_BUFFER_" + vera::toString(i));
            m_doubleBuffers_shaders[i].load(m_frag_source, vera::getDefaultSrc(vera::VERT_BILLBOARD), vera::SHOW_MAGENTA_SHADER, false);
        }
    }
    else {
        for (size_t i = 0; i < m_doubleBuffers_shaders.size(); i++) {

            // Reload shader code
            m_doubleBuffers_shaders[i].addDefine("DOUBLE_BUFFER_" + vera::toString(i));
            m_doubleBuffers_shaders[i].load(m_frag_source, vera::getDefaultSrc(vera::VERT_BILLBOARD), vera::SHOW_MAGENTA_SHADER, false);
        }
    }

    if ( m_pyramid_total != int(uniforms.pyramids.size()) ) {

        if (verbose)
            std::cout << "Removing " << uniforms.pyramids.size() << " convolution pyramids to create  " << m_pyramid_total << std::endl;

        uniforms.pyramids.clear();
        m_pyramid_fbos.clear();
        m_pyramid_subshaders.clear();
        for (int i = 0; i < m_pyramid_total; i++) {
            glm::vec2 size = glm::vec2(vera::getWindowWidth(), vera::getWindowHeight());
            bool fixed = getBufferSize(m_frag_source, "u_pyramid" + vera::toString(i), size);
            
            uniforms.pyramids.push_back( vera::Pyramid() );
            uniforms.pyramids[i].allocate(size.x, size.y);
            uniforms.pyramids[i].fixed = fixed;
            uniforms.pyramids[i].pass = [this](vera::Fbo *_target, const vera::Fbo *_tex0, const vera::Fbo *_tex1, int _depth) {
                _target->bind();
                glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
                glClear(GL_COLOR_BUFFER_BIT);
                m_pyramid_shader.use();

                uniforms.feedTo( &m_pyramid_shader);

                m_pyramid_shader.setUniform("u_pyramidDepth", _depth);
                m_pyramid_shader.setUniform("u_pyramidTotalDepth", (int)uniforms.pyramids[0].getDepth());
                m_pyramid_shader.setUniform("u_pyramidUpscaling", _tex1 != NULL);

                m_pyramid_shader.textureIndex = (uniforms.models.size() == 0) ? 1 : 0;
                m_pyramid_shader.setUniformTexture("u_pyramidTex0", _tex0);
                if (_tex1 != NULL)
                    m_pyramid_shader.setUniformTexture("u_pyramidTex1", _tex1);
                m_pyramid_shader.setUniform("u_resolution", ((float)_target->getWidth()), ((float)_target->getHeight()));
                m_pyramid_shader.setUniform("u_pixel", 1.0f/((float)_target->getWidth()), 1.0f/((float)_target->getHeight()));

                vera::getBillboard()->render( &m_pyramid_shader );
                _target->unbind();
            };
            m_pyramid_fbos.push_back( vera::Fbo() );
            m_pyramid_fbos[i].allocate(size.x, size.y, vera::COLOR_TEXTURE);
            m_pyramid_fbos[i].fixed = fixed;
            m_pyramid_subshaders.push_back( vera::Shader() );
        }
    }
    
    if ( checkConvolutionPyramid( getSource(FRAGMENT) ) ) {
        m_pyramid_shader.addDefine("CONVOLUTION_PYRAMID_ALGORITHM");
        m_pyramid_shader.load(m_frag_source, vera::getDefaultSrc(vera::VERT_BILLBOARD), vera::SHOW_MAGENTA_SHADER, false);
    }
    else
        m_pyramid_shader.load(vera::getDefaultSrc(vera::FRAG_POISSON), vera::getDefaultSrc(vera::VERT_BILLBOARD), vera::SHOW_MAGENTA_SHADER, false);

    for (size_t i = 0; i < m_pyramid_subshaders.size(); i++) {
        m_pyramid_subshaders[i].addDefine("CONVOLUTION_PYRAMID_" + vera::toString(i));
        m_pyramid_subshaders[i].load(m_frag_source, vera::getDefaultSrc(vera::VERT_BILLBOARD), vera::SHOW_MAGENTA_SHADER, false);
    }
}

// ------------------------------------------------------------------------- DRAW
void Sandbox::_renderBuffers() {
    glDisable(GL_BLEND);

    bool reset_viewport = false;
    for (size_t i = 0; i < uniforms.buffers.size(); i++) {
        TRACK_BEGIN("render:buffer" + vera::toString(i))

        reset_viewport += uniforms.buffers[i].fixed;

        uniforms.buffers[i].bind();

        m_buffers_shaders[i].use();

        // Pass textures for the other buffers
        for (size_t j = 0; j < uniforms.buffers.size(); j++)
            if (i != j)
                m_buffers_shaders[i].setUniformTexture("u_buffer" + vera::toString(j), &uniforms.buffers[j] );

        for (size_t j = 0; j < uniforms.doubleBuffers.size(); j++)
                m_buffers_shaders[i].setUniformTexture("u_doubleBuffer" + vera::toString(j), uniforms.doubleBuffers[j].src );

        // Update uniforms and textures
        uniforms.feedTo( &m_buffers_shaders[i], true, false);

        vera::getBillboard()->render( &m_buffers_shaders[i] );
        
        uniforms.buffers[i].unbind();

        TRACK_END("render:buffer" + vera::toString(i))
    }

    for (size_t i = 0; i < uniforms.doubleBuffers.size(); i++) {
        TRACK_BEGIN("render:doubleBuffer" + vera::toString(i))

        reset_viewport += uniforms.doubleBuffers[i].src->fixed;

        uniforms.doubleBuffers[i].dst->bind();

        m_doubleBuffers_shaders[i].use();

        // Pass textures for the other buffers
        for (size_t j = 0; j < uniforms.buffers.size(); j++)
            m_doubleBuffers_shaders[i].setUniformTexture("u_buffer" + vera::toString(j), &uniforms.buffers[j] );

        for (size_t j = 0; j < uniforms.doubleBuffers.size(); j++)
            m_doubleBuffers_shaders[i].setUniformTexture("u_doubleBuffer" + vera::toString(j), uniforms.doubleBuffers[j].src );

        // Update uniforms and textures
        uniforms.feedTo( &m_doubleBuffers_shaders[i], true, false);

        vera::getBillboard()->render( &m_doubleBuffers_shaders[i] );
        
        uniforms.doubleBuffers[i].dst->unbind();
        uniforms.doubleBuffers[i].swap();

        TRACK_END("render:doubleBuffer" + vera::toString(i))
    }

    for (size_t i = 0; i < m_pyramid_subshaders.size(); i++) {
        TRACK_BEGIN("render:convolution_pyramid" + vera::toString(i))

        reset_viewport += m_pyramid_fbos[i].fixed;

        m_pyramid_fbos[i].bind();
        m_pyramid_subshaders[i].use();

        // Clear the background
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        // Update uniforms and textures
        uniforms.feedTo( &m_pyramid_subshaders[i] );
        vera::getBillboard()->render( &m_pyramid_subshaders[i] );

        m_pyramid_fbos[i].unbind();

        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        uniforms.pyramids[i].process(&m_pyramid_fbos[i]);

        TRACK_END("render:convolution_pyramid" + vera::toString(i))
    }

    #if defined(__EMSCRIPTEN__)
    if (vera::getWebGLVersionNumber() == 1)
        reset_viewport = true;
    #endif

    if (reset_viewport)
        glViewport(0.0f, 0.0f, vera::getWindowWidth(), vera::getWindowHeight());
    
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}

void Sandbox::renderPrep() {
    TRACK_BEGIN("render")

    // UPDATE STREAMING TEXTURES
    // -----------------------------------------------
    if (m_initialized)
        uniforms.update();

    // RENDER SHADOW MAP
    // -----------------------------------------------
    if (uniforms.models.size() > 0)
        m_sceneRender.renderShadowMap(uniforms);
    
    // BUFFERS
    // -----------------------------------------------
    if (uniforms.buffers.size() > 0 || 
        uniforms.doubleBuffers.size() > 0 ||
        m_pyramid_total > 0)
        _renderBuffers();
    
    // MAIN SCENE
    // ----------------------------------------------- < main scene start
    if (screenshotFile != "" || isRecording() )
        if (!m_record_fbo.isAllocated())
            m_record_fbo.allocate(vera::getWindowWidth(), vera::getWindowHeight(), vera::COLOR_TEXTURE_DEPTH_BUFFER);

    if (m_postprocessing || m_plot == PLOT_LUMA || m_plot == PLOT_RGB || m_plot == PLOT_RED || m_plot == PLOT_GREEN || m_plot == PLOT_BLUE ) {
        _updateSceneBuffer(vera::getWindowWidth(), vera::getWindowHeight());
        m_sceneRender_fbo.bind();
    }
    else if (screenshotFile != "" || isRecording() )
        m_record_fbo.bind();

    // Clear the background
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

void Sandbox::render() {
    // RENDER CONTENT
    if (uniforms.models.size() == 0) {
        TRACK_BEGIN("render:billboard")

        // Load main shader
        m_canvas_shader.use();

        if (quilt >= 0) {
            vera::renderQuilt([&](const vera::QuiltProperties& quilt, glm::vec4& viewport, int &viewIndex) {

                // set up the camera rotation and position for current view
                uniforms.activeCamera->setVirtualOffset(5.0f, viewIndex, quilt.totalViews);
                uniforms.set("u_tile", float(quilt.columns), float(quilt.rows), float(quilt.totalViews));
                uniforms.set("u_viewport", float(viewport.x), float(viewport.y), float(viewport.z), float(viewport.w));

                // Update Uniforms and textures variables
                uniforms.feedTo( &m_canvas_shader );

                // Pass special uniforms
                m_canvas_shader.setUniform("u_modelViewProjectionMatrix", glm::mat4(1.));
                vera::getBillboard()->render( &m_canvas_shader );
            }, true);
        }

        else {
            // Update Uniforms and textures variables
            uniforms.feedTo( &m_canvas_shader );

            // Pass special uniforms
            m_canvas_shader.setUniform("u_modelViewProjectionMatrix", glm::mat4(1.));
            vera::getBillboard()->render( &m_canvas_shader );
        }

        TRACK_END("render:billboard")
    }

    else {
        TRACK_BEGIN("render:scene")
        if (quilt >= 0) {
            vera::renderQuilt([&](const vera::QuiltProperties& quilt, glm::vec4& viewport, int &viewIndex){

                // set up the camera rotation and position for current view
                uniforms.activeCamera->setVirtualOffset(m_sceneRender.getArea() * 0.75, viewIndex, quilt.totalViews);
                // uniforms.activeCamera->setVirtualOffset(5.0f, viewIndex, quilt.totalViews);
                // uniforms.activeCamera->setVirtualOffset(10.0f, viewIndex, quilt.totalViews);

                uniforms.set("u_tile", float(quilt.columns), float(quilt.rows), float(quilt.totalViews));
                uniforms.set("u_viewport", float(viewport.x), float(viewport.y), float(viewport.z), float(viewport.w));

                m_sceneRender.render(uniforms);

                if (m_sceneRender.showGrid || m_sceneRender.showAxis || m_sceneRender.showBBoxes)
                    m_sceneRender.renderDebug(uniforms);
            }, true);
        }

        else {
            m_sceneRender.render(uniforms);
            if (m_sceneRender.showGrid || m_sceneRender.showAxis || m_sceneRender.showBBoxes)
                m_sceneRender.renderDebug(uniforms);
        }
        TRACK_END("render:scene")
    }
}

void Sandbox::renderPost() {

    // POST PROCESSING
    if (m_postprocessing) {
        TRACK_BEGIN("render:postprocessing")

        m_sceneRender_fbo.unbind();

        if (screenshotFile != "" || isRecording())
            m_record_fbo.bind();
    
        m_postprocessing_shader.use();

        // Update uniforms and textures
        uniforms.feedTo( &m_postprocessing_shader );

        if (lenticular.size() > 0)
            feedLenticularUniforms(m_postprocessing_shader);

        vera::getBillboard()->render( &m_postprocessing_shader );

        TRACK_END("render:postprocessing")
    }
    else if (m_plot == PLOT_RGB || m_plot == PLOT_RED || m_plot == PLOT_GREEN || m_plot == PLOT_BLUE || m_plot == PLOT_LUMA) {
        m_sceneRender_fbo.unbind();

        if (screenshotFile != "" || isRecording())
            m_record_fbo.bind();

        vera::image(m_sceneRender_fbo);
    }
    
    if (screenshotFile != "" || isRecording()) {
        m_record_fbo.unbind();

        vera::image(m_record_fbo);
    }

    TRACK_END("render")
    console_uniforms_refresh();
}


void Sandbox::renderUI() {
    // TRACK_BEGIN("renderUI")

    // // IN PUT TEXTURES
    if (m_showTextures) {      

        int nTotal = uniforms.textures.size();
        if (nTotal > 0) {
            glDisable(GL_DEPTH_TEST);
            // TRACK_BEGIN("textures")
            float w = (float)(vera::getWindowWidth());
            float h = (float)(vera::getWindowHeight());
            float scale = fmin(1.0f / (float)(nTotal), 0.25) * 0.5;
            float xStep = w * scale;
            float yStep = h * scale;
            float xOffset = xStep;
            float yOffset = h - yStep;

            vera::textAngle(-HALF_PI);
            vera::textAlign(vera::ALIGN_TOP);
            vera::textAlign(vera::ALIGN_LEFT);
            vera::textSize(yStep * 0.2f);

            for (vera::TexturesMap::iterator it = uniforms.textures.begin(); it != uniforms.textures.end(); it++) {
                vera::TextureStreamsMap::const_iterator slit = uniforms.streams.find(it->first);
                if ( slit != uniforms.streams.end() )
                    vera::image((vera::TextureStream*)slit->second, xOffset, yOffset, xStep, yStep, true);
                else 
                    vera::image(it->second, xOffset, yOffset, xStep, yStep);

                vera::text(it->first, xOffset + xStep, vera::getWindowHeight() - yOffset + yStep);

                yOffset -= yStep * 2.0;
            }
    //         // TRACK_END("textures")
        }
    }

    // RESULTING BUFFERS
    if (m_showPasses) {        

        glDisable(GL_DEPTH_TEST);
        // TRACK_BEGIN("buffers")

        // DEBUG BUFFERS
        int nTotal = uniforms.buffers.size();
        if (m_doubleBuffers_total > 0)
            nTotal += uniforms.doubleBuffers.size();
        if (m_pyramid_total > 0)
            nTotal += uniforms.pyramids.size();
        nTotal += uniforms.functions["u_scene"].present;
        nTotal += uniforms.functions["u_sceneDepth"].present;
        nTotal += (uniforms.models.size() > 0);

        if (nTotal > 0) {
            float w = (float)(vera::getWindowWidth());
            float h = (float)(vera::getWindowHeight());
            float scale = fmin(1.0f / (float)(nTotal), 0.25) * 0.5;
            float xStep = w * scale;
            float yStep = h * scale;
            float xOffset = w - xStep;
            float yOffset = h - yStep;

            vera::textAngle(-HALF_PI);
            vera::textSize(yStep * 0.2f);
            vera::textAlign(vera::ALIGN_BOTTOM);
            vera::textAlign(vera::ALIGN_LEFT);

            for (size_t i = 0; i < uniforms.buffers.size(); i++) {
                glm::vec2 offset = glm::vec2(xOffset, yOffset);
                glm::vec2 scale = glm::vec2(yStep);
                scale.x *= ((float)uniforms.buffers[i].getWidth()/(float)uniforms.buffers[i].getHeight());
                offset.x += xStep - scale.x;

                vera::image(uniforms.buffers[i], offset.x, offset.y, scale.x, scale.y);
                vera::text("u_buffer" + vera::toString(i), xOffset - scale.x, vera::getWindowHeight() - yOffset + yStep);

                yOffset -= yStep * 2.0;
            }

            for (size_t i = 0; i < uniforms.doubleBuffers.size(); i++) {
                glm::vec2 offset = glm::vec2(xOffset, yOffset);
                glm::vec2 scale = glm::vec2(yStep);
                scale.x *= ((float)uniforms.doubleBuffers[i].src->getWidth()/(float)uniforms.doubleBuffers[i].src->getHeight());
                offset.x += xStep - scale.x;

                vera::image(uniforms.doubleBuffers[i].src, offset.x, offset.y, scale.x, scale.y);
                vera::text("u_doubleBuffer" + vera::toString(i), xOffset - scale.x, vera::getWindowHeight() - yOffset + yStep);

                yOffset -= yStep * 2.0;
            }

            for (size_t i = 0; i < uniforms.pyramids.size(); i++) {
                glm::vec2 offset = glm::vec2(xOffset, yOffset);
                glm::vec2 scale = glm::vec2(yStep);
                scale.x *= ((float)uniforms.pyramids[i].getWidth()/(float)uniforms.pyramids[i].getHeight());
                float w = scale.x;
                offset.x += xStep - w;
                for (size_t j = 0; j < uniforms.pyramids[i].getDepth() * 2; j++ ) {

                    if (j < uniforms.pyramids[i].getDepth())
                        vera::image(uniforms.pyramids[i].getResult(j), offset.x, offset.y, scale.x, scale.y);
                    else 
                        vera::image(uniforms.pyramids[i].getResult(j), offset.x + w * 2.0f, offset.y, scale.x, scale.y);

                    offset.x -= scale.x;
                    if (j < uniforms.pyramids[i].getDepth()) {
                        scale *= 0.5;
                        offset.y = yOffset - yStep * 0.5;
                    }
                    else {
                        offset.y = yOffset + yStep * 0.5;
                        scale *= 2.0;
                    }
                    offset.x -= scale.x;

                }

                // vera::text("u_pyramid0" + vera::toString(i), xOffset - scale.x * 2.0, vera::getWindowHeight() - yOffset + yStep);
                yOffset -= yStep * 2.0;

            }

            if (m_postprocessing) {
                if (uniforms.functions["u_scene"].present) {
                    vera::image(&m_sceneRender_fbo, xOffset, yOffset, xStep, yStep);
                    vera::text("u_scene", xOffset - xStep, vera::getWindowHeight() - yOffset + yStep);
                    yOffset -= yStep * 2.0;
                }

                if (uniforms.functions["u_sceneDepth"].present) {
                    if (uniforms.activeCamera)
                        vera::imageDepth(&m_sceneRender_fbo, xOffset, yOffset, xStep, yStep, uniforms.activeCamera->getFarClip(), uniforms.activeCamera->getNearClip());
                    vera::text("u_sceneDepth", xOffset - xStep, vera::getWindowHeight() - yOffset + yStep);
                    yOffset -= yStep * 2.0;
                }
            }

            if (uniforms.models.size() > 0) {
                for (vera::LightsMap::iterator it = uniforms.lights.begin(); it != uniforms.lights.end(); ++it ) {
                    if ( it->second->getShadowMap()->getDepthTextureId() ) {
                        vera::imageDepth(it->second->getShadowMap(), xOffset, yOffset, xStep, yStep, it->second->getShadowMapFar(), it->second->getShadowMapNear());
                        // vera::image(it->second->getShadowMap(), xOffset, yOffset, xStep, yStep);
                        vera::text("u_lightShadowMap", xOffset - xStep, vera::getWindowHeight() - yOffset + yStep);
                        yOffset -= yStep * 2.0;
                    }
                }
            }
        }
        // TRACK_END("buffers")
    }

    if (m_plot != PLOT_OFF && m_plot_texture ) {
        glDisable(GL_DEPTH_TEST);
        //TRACK_BEGIN("plot_data")

        float p = vera::getPixelDensity();
        float w = 100 * p;
        float h = 30 * p;
        float x = (float)(vera::getWindowWidth()) * 0.5;
        float y = h + 10;

        if (!m_plot_shader.loaded())
            m_plot_shader.load(vera::getDefaultSrc(vera::FRAG_PLOT), vera::getDefaultSrc(vera::VERT_DYNAMIC_BILLBOARD), vera::SHOW_MAGENTA_SHADER, false);

        m_plot_shader.use();
        m_plot_shader.setUniform("u_scale", w, h);
        m_plot_shader.setUniform("u_translate", x, y);
        m_plot_shader.setUniform("u_resolution", (float)vera::getWindowWidth(), (float)vera::getWindowHeight());
        m_plot_shader.setUniform("u_viewport", w, h);
        m_plot_shader.setUniform("u_modelViewProjectionMatrix", vera::getOrthoMatrix());
        m_plot_shader.setUniformTexture("u_plotData", m_plot_texture, 0);
        vera::getBillboard()->render(&m_plot_shader);
        // TRACK_END("plot_data")
    }

    if (cursor && vera::getMouseEntered()) {
        // TRACK_BEGIN("cursor")
        if (m_cross_vbo == nullptr) 
            m_cross_vbo = new vera::Vbo( vera::crossMesh( glm::vec3(0.0f, 0.0f, 0.0f), 10.0f) );

        vera::Shader* fill = vera::getFillShader();
        fill->use();
        fill->setUniform("u_modelViewProjectionMatrix", glm::translate(vera::getOrthoMatrix(), glm::vec3(vera::getMouseX(), vera::getMouseY(), 0.0f) ) );
        fill->setUniform("u_color", glm::vec4(1.0f));
        m_cross_vbo->render(fill);
        // TRACK_END("cursor")
    }

    // TRACK_END("renderUI")
}

void Sandbox::renderDone() {
    // TRACK_BEGIN("update:post_render")

    // RECORD
    if (isRecording()) {
        onScreenshot( vera::toString( getRecordingCount() , 0, 5, '0') + ".png");
        recordingFrameAdded();
    }
    // SCREENSHOT 
    else if (screenshotFile != "") {
        onScreenshot(screenshotFile);
        screenshotFile = "";
    }

    unflagChange();

    if (m_plot != PLOT_OFF)
        onPlot();

    if (!m_initialized) {
        m_initialized = true;
        vera::updateViewport();
        flagChange();
    }
    else {
        m_frame++;
    }

    // TRACK_END("update:post_render")
}

// ------------------------------------------------------------------------- ACTIONS

void Sandbox::clear() {
    uniforms.clear();

    if (uniforms.models.size() > 0)
        m_sceneRender.clear();

    if (m_cross_vbo)
        delete m_cross_vbo;
}

void Sandbox::printDependencies(ShaderType _type) const {
    if (_type == FRAGMENT) {
        for (size_t i = 0; i < m_frag_dependencies.size(); i++) {
            std::cout << m_frag_dependencies[i] << std::endl;
        }
    }
    else {
        for (size_t i = 0; i < m_vert_dependencies.size(); i++) {
            std::cout << m_vert_dependencies[i] << std::endl;
        }
    }
}

// ------------------------------------------------------------------------- EVENTS

void Sandbox::onFileChange(WatchFileList &_files, int index) {
    console_clear();
    
    FileType type = _files[index].type;
    std::string filename = _files[index].path;

    // IF the change is on a dependency file, re route to the correct shader that need to be reload
    if (type == GLSL_DEPENDENCY) {
        if (std::find(m_frag_dependencies.begin(), m_frag_dependencies.end(), filename) != m_frag_dependencies.end()) {
            type = FRAG_SHADER;
            filename = _files[frag_index].path;
        }
        else if(std::find(m_vert_dependencies.begin(), m_vert_dependencies.end(), filename) != m_vert_dependencies.end()) {
            type = VERT_SHADER;
            filename = _files[vert_index].path;
        }
    }
    
    if (type == FRAG_SHADER) {
        m_frag_source = "";
        m_frag_dependencies.clear();
        if ( vera::loadGlslFrom(filename, &m_frag_source, include_folders, &m_frag_dependencies) )
            reloadShaders(_files);
    }
    else if (type == VERT_SHADER) {
        m_vert_source = "";
        m_vert_dependencies.clear();
        if ( vera::loadGlslFrom(filename, &m_vert_source, include_folders, &m_vert_dependencies) )
            reloadShaders(_files);
    }
    else if (type == GEOMETRY) {
        // TODO
    }
    else if (type == IMAGE) {
        for (vera::TexturesMap::iterator it = uniforms.textures.begin(); it!=uniforms.textures.end(); it++) {
            if (filename == it->second->getFilePath()) {
                std::cout << "Reloading" << filename << std::endl;
                it->second->load(filename, _files[index].vFlip);
                break;
            }
        }
    }
    else if (type == CUBEMAP) {
        for (vera::TextureCubesMap::iterator it = uniforms.cubemaps.begin(); it!=uniforms.cubemaps.end(); it++) {
            if (filename == it->second->getFilePath()) {
                std::cout << "Reloading" << filename << std::endl;
                it->second->load(filename, _files[index].vFlip);
                break;
            }
        }
    }

    flagChange();
}

void Sandbox::onScroll(float _yoffset) {
    // Vertical scroll button zooms u_view2d and view3d.
    /* zoomfactor 2^(1/4): 4 scroll wheel clicks to double in size. */
    constexpr float zoomfactor = 1.1892;
    if (_yoffset != 0) {
        float z = pow(zoomfactor, _yoffset);

        // zoom view2d
        glm::vec2 zoom = glm::vec2(z,z);
        glm::vec2 origin = {vera::getWindowWidth()/2, vera::getWindowHeight()/2};
        m_view2d = glm::translate(m_view2d, origin);
        m_view2d = glm::scale(m_view2d, zoom);
        m_view2d = glm::translate(m_view2d, -origin);
        
        flagChange();
    }
}

void Sandbox::onMouseDrag(float _x, float _y, int _button) {
    if (uniforms.activeCamera == nullptr)
        return;

    if (quilt < 0) {
        // If it's not playing on the HOLOPLAY
        // produce continue draging like blender
        //
        float x = _x;
        float y = _y;
        float pd = vera::getPixelDensity();

        if (x <= 0) x = vera::getWindowWidth() - 2;
        else if (x >= vera::getWindowWidth()) x = 2; 

        if (y <= 0) y = vera::getWindowHeight() - 2;
        else if (y >= vera::getWindowHeight()) y = 2;

        if (x != _x || y != _y) vera::setMousePosition(x, y);
    }

    if (_button == 1) {
        // Left-button drag is used to pan u_view2d.
        m_view2d = glm::translate(m_view2d, glm::vec2(-vera::getMouseVelX(),-vera::getMouseVelY()) );

        // Left-button drag is used to rotate geometry.
        float dist = uniforms.activeCamera->getDistance();

        float vel_x = vera::getMouseVelX();
        float vel_y = vera::getMouseVelY();

        if (fabs(vel_x) < 50.0 && fabs(vel_y) < 50.0) {
            m_camera_azimuth -= vel_x;
            m_camera_elevation -= vel_y * 0.5;
            uniforms.activeCamera->orbit(m_camera_azimuth, m_camera_elevation, dist);
            uniforms.activeCamera->lookAt(glm::vec3(0.0));
        }
    } 
    else {
        // Right-button drag is used to zoom geometry.
        float dist = uniforms.activeCamera->getDistance();
        dist += (-.008f * vera::getMouseVelY());
        if (dist > 0.0f) {
            uniforms.activeCamera->orbit(m_camera_azimuth, m_camera_elevation, dist);
            uniforms.activeCamera->lookAt(glm::vec3(0.0));
        }
    }
}

void Sandbox::onViewportResize(int _newWidth, int _newHeight) {
    if (uniforms.activeCamera)
        uniforms.activeCamera->setViewport(_newWidth, _newHeight);
    
    for (size_t i = 0; i < uniforms.buffers.size(); i++) 
        if (!uniforms.buffers[i].fixed)
            uniforms.buffers[i].allocate(_newWidth, _newHeight, vera::COLOR_TEXTURE);

    for (size_t i = 0; i < uniforms.doubleBuffers.size(); i++) {
        if (!uniforms.doubleBuffers[i][0].fixed)
            uniforms.doubleBuffers[i][0].allocate(_newWidth, _newHeight, vera::COLOR_TEXTURE);
        if (!uniforms.doubleBuffers[i][1].fixed)
            uniforms.doubleBuffers[i][1].allocate(_newWidth, _newHeight, vera::COLOR_TEXTURE);
    }

    for (size_t i = 0; i < uniforms.pyramids.size(); i++) {
        if (!m_pyramid_fbos[i].fixed) {
            m_pyramid_fbos[i].allocate(_newWidth, _newHeight, vera::COLOR_TEXTURE);
            uniforms.pyramids[i].allocate(vera::getWindowWidth(), vera::getWindowHeight());
        }
    }

    if (m_postprocessing || m_plot == PLOT_LUMA || m_plot == PLOT_RGB || m_plot == PLOT_RED || m_plot == PLOT_GREEN || m_plot == PLOT_BLUE )
        _updateSceneBuffer(_newWidth, _newHeight);

    if (screenshotFile != "" || isRecording())
        m_record_fbo.allocate(_newWidth, _newHeight, vera::COLOR_TEXTURE_DEPTH_BUFFER);

    flagChange();
}

void Sandbox::onScreenshot(std::string _file) {

    if (_file != "" && vera::isGL()) {
        // TRACK_BEGIN("screenshot")

        glBindFramebuffer(GL_FRAMEBUFFER, m_record_fbo.getId());

        if (vera::getExt(_file) == "hdr") {
            float* pixels = new float[vera::getWindowWidth() * vera::getWindowHeight()*4];
            glReadPixels(0, 0, vera::getWindowWidth(), vera::getWindowHeight(), GL_RGBA, GL_FLOAT, pixels);
            vera::savePixelsHDR(_file, pixels, vera::getWindowWidth(), vera::getWindowHeight());
        }
        #if defined(SUPPORT_LIBAV) && !defined(PLATFORM_RPI)
        else if (recordingPipe()) {
            int width = vera::getWindowWidth();
            int height = vera::getWindowHeight();
            auto pixels = std::unique_ptr<unsigned char[]>(new unsigned char [width * height * 3]);
            glReadPixels(0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, pixels.get());
            recordingPipeFrame( std::move(pixels) );
        }
        #endif
        else {
            int width = vera::getWindowWidth();
            int height = vera::getWindowHeight();
            auto pixels = std::unique_ptr<unsigned char[]>(new unsigned char [width * height * 4]);
            glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.get());

            #if defined(SUPPORT_MULTITHREAD_RECORDING)
            
            std::shared_ptr<Job> saverPtr = std::make_shared<Job>(std::move(_file), width, height, std::move(pixels), m_task_count, m_max_mem_in_queue);
            /** In the case that we render faster than we can safe frames, more and more frames
             * have to be stored temporary in the save queue. That means that more and more ram is used.
             * If to much is memory is used, we save the current frame directly to prevent that the system
             * is running out of memory. Otherwise we put the frame in to the thread queue, so that we can utilize
             * multilple cpu cores */
            if (m_max_mem_in_queue <= 0) {
                Job& saver = *saverPtr;
                saver();
            }
            else {
                auto func = [saverPtr]() {
                    Job& saver = *saverPtr;
                    saver();
                };
                m_save_threads.Submit(std::move(func));
            }
            #else

            vera::savePixels(_file, pixels.get(), width, height);

            #endif
        }
    
        if ( !isRecording() )
            std::cout << "Screenshot saved to " << _file << std::endl;

        glBindFramebuffer(GL_FRAMEBUFFER, 0);

        // TRACK_END("screenshot")
    }
}

void Sandbox::onPlot() {
    if ( !vera::isGL() )
        return;

    if ( (m_plot == PLOT_LUMA || m_plot == PLOT_RGB || m_plot == PLOT_RED || m_plot == PLOT_GREEN || m_plot == PLOT_BLUE ) && haveChange() ) {

        // TRACK_BEGIN("plot::histogram")

        // Extract pixels
        glBindFramebuffer(GL_FRAMEBUFFER, m_sceneRender_fbo.getId());
        int w = vera::getWindowWidth();
        int h = vera::getWindowHeight();
        int c = 3;
        int total = w * h * c;
        unsigned char* pixels = new unsigned char[total];
        glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, pixels);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);

        // Count frequencies of appearances 
        float max_rgb_freq = 0;
        float max_luma_freq = 0;
        glm::vec4 freqs[256];
        for (int i = 0; i < total; i += c) {
            m_plot_values[pixels[i]].r++;
            if (m_plot_values[pixels[i]].r > max_rgb_freq)
                max_rgb_freq = m_plot_values[pixels[i]].r;

            m_plot_values[pixels[i+1]].g++;
            if (m_plot_values[pixels[i+1]].g > max_rgb_freq)
                max_rgb_freq = m_plot_values[pixels[i+1]].g;

            m_plot_values[pixels[i+2]].b++;
            if (m_plot_values[pixels[i+2]].b > max_rgb_freq)
                max_rgb_freq = m_plot_values[pixels[i+2]].b;

            int luma = 0.299 * pixels[i] + 0.587 * pixels[i+1] + 0.114 * pixels[i+2];
            m_plot_values[luma].a++;
            if (m_plot_values[luma].a > max_luma_freq)
                max_luma_freq = m_plot_values[luma].a;
        }
        delete[] pixels;

        // Normalize frequencies
        for (int i = 0; i < 256; i ++)
            m_plot_values[i] = m_plot_values[i] / glm::vec4(max_rgb_freq, max_rgb_freq, max_rgb_freq, max_luma_freq);

        if (m_plot_texture == nullptr)
            m_plot_texture = new vera::Texture();
        m_plot_texture->load(256, 1, 4, 32, &m_plot_values[0], vera::NEAREST, vera::CLAMP);

        uniforms.textures["u_histogram"] = m_plot_texture;
        uniforms.flagChange();
        // TRACK_END("plot::histogram")
    }

    else if (m_plot == PLOT_FPS ) {
        // Push back values one position
        for (int i = 0; i < 255; i ++)
            m_plot_values[i] = m_plot_values[i+1];
        m_plot_values[255] = glm::vec4( vera::getFps()/60.0f, 0.0f, 0.0f, 1.0f);

        // TRACK_BEGIN("plot::fps")

        if (m_plot_texture == nullptr)
            m_plot_texture = new vera::Texture();

        m_plot_texture->load(256, 1, 4, 32, &m_plot_values[0], vera::NEAREST, vera::CLAMP);
        // uniforms.textures["u_sceneFps"] = m_plot_texture;

        // TRACK_END("plot::fps")
    }

    else if (m_plot == PLOT_MS ) {
        // Push back values one position
        for (int i = 0; i < 255; i ++)
            m_plot_values[i] = m_plot_values[i+1];
        m_plot_values[255] = glm::vec4( vera::getDelta(), 0.0f, 0.0f, 1.0f);

        // TRACK_BEGIN("plot::ms")

        if (m_plot_texture == nullptr)
            m_plot_texture = new vera::Texture();

        m_plot_texture->load(256, 1, 4, 32, &m_plot_values[0], vera::NEAREST, vera::CLAMP);

        // uniforms.textures["u_sceneMs"] = m_plot_texture;

        // TRACK_END("plot::ms")
    }
}
