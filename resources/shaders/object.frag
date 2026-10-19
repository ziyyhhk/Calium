#version 330 core

out vec4 FragColor;

in vec4 t_color;
in vec2 t_texCoord;

uniform sampler2D u_texture;

void main() {
    FragColor = texture(u_texture, t_texCoord) * t_color;
}
