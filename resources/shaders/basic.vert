#version 330 core

layout (location = 0) in vec2 a_pos;
layout (location = 1) in vec4 a_color;

out vec4 t_color;

uniform mat4 u_mvp;

void main() {
    gl_Position = u_mvp * vec4(a_pos, 0.0, 1.0);
    t_color = a_color;
}
