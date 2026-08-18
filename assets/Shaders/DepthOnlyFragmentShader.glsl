#version 420 core
// Deliberately empty. A cascade's framebuffer has no colour attachment — GL sets its draw and read
// buffers to GL_NONE — so a fragment output here would have nowhere to go; the only thing this
// stage produces is the depth the rasteriser already computed.

void main()
{
}
