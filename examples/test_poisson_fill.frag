
#ifdef GL_ES
precision mediump float;
#endif

uniform sampler2D   u_tex0;
uniform vec2        u_tex0Resolution;
uniform vec2        u_resolution;

uniform sampler2D   u_pyramid0;
uniform sampler2D   u_pyramidTex0;
uniform sampler2D   u_pyramidTex1;
uniform bool        u_pyramidUpscaling;

const vec3  h1      = vec3(1.0334, 0.6836, 0.1507);
const float h2      = 0.0270;
const vec2  g       = vec2(0.7753, 0.0312);

#define saturate(x) clamp(x, 0.0, 1.0)
#define absi(x)     ( (x < 0)? x * -1 : x )

void main (void) {
    vec4 color = vec4(0.0);
    vec2 st = gl_FragCoord.xy/u_resolution;

#if defined(CONVOLUTION_PYRAMID_0)
    color = texture2D(u_tex0, st);

// #elif defined(CONVOLUTION_PYRAMID_ALGORITHM)
// // Buy default CONVOLUTION_PYRAMID_ALGORITHM looks like this:
//     vec2 pixel = 1.0/u_resolution;

//     st -= pixel * 0.5;

//     if (!u_pyramidUpscaling) {
//         for (int dy = -2; dy <= 2; dy++) {
//             for (int dx = -2; dx <= 2; dx++) {
//                 vec2 uv = st + vec2(float(dx), float(dy)) * pixel * 0.5;
//                 if (uv.x <= 0.0 || uv.x >= 1.0 || uv.y <= 0.0 || uv.y >= 1.0)
//                     continue;
//                 color += texture2D(u_pyramidTex0, saturate(uv)) * h1[ absi(dx) ] * h1[ absi(dy) ];
//             }
//         }
//     }
//     else {
//         for (int dy = -1; dy <= 1; dy++) {
//             for (int dx = -1; dx <= 1; dx++) {
//                 vec2 uv = st + vec2(float(dx), float(dy)) * pixel;
//                 color += texture2D(u_pyramidTex0, saturate(uv)) * g[ absi(dx) ] * g[ absi(dy) ];
//             }
//         }

//         for (int dy = -2; dy <= 2; dy++) {
//             for (int dx = -2; dx <= 2; dx++) {
//                 vec2 uv = st + vec2(float(dx), float(dy)) * 2.0 * pixel;
//                 if (uv.x <= 0.0 || uv.x >= 1.0 || uv.y <= 0.0 || uv.y >= 1.0)
//                     continue;
//                 color += texture2D(u_pyramidTex1, saturate(uv)) * h2 * h1[ absi(dx) ] * h1[ absi(dy) ];
//             }
//         }
//     }

//     color = (color.a == 0.0)? color : vec4(color.rgb/color.a, 1.0);
#else
    color = texture2D(u_pyramid0, st);

#endif

    gl_FragColor = color;
}
