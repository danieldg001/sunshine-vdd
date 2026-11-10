#version 300 es
#extension GL_OES_EGL_image_external : enable
#extension GL_OES_EGL_image_external_essl3 : require

precision highp float;

in vec2 texcoords_out;
uniform samplerExternalOES tex1;
uniform vec2 uv_offset;
uniform vec2 uv_scale;
out vec4 FragColor;

void main()
{
    FragColor = texture(tex1, uv_offset + texcoords_out * uv_scale);
}
