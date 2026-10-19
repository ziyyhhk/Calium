#version 330 core

out vec4 FragColor;

in vec4 t_color;

void main() {
    FragColor = t_color;
}
