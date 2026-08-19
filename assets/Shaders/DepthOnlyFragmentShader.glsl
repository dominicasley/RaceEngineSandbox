#version 450
// Deliberately empty, and deliberately declaring no output: a cascade's VkRenderingInfo records
// colorAttachmentCount 0 and the pipeline is built with no colour blend attachment, so a location
// written here would have no attachment to be written to.

void main()
{
}
