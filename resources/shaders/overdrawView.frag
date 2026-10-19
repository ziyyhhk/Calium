#version 330 core

out vec4 FragColor;

in vec2 t_texCoord;

uniform sampler2D u_frame;
uniform float u_intensity;

void main() {
    vec4 col = texture(u_frame, t_texCoord);
    float intensity = col.a * u_intensity;
    FragColor = vec4(intensity, intensity * 0.5, 0.0, 1.0);
}
