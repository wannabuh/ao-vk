#version 450
// The ground grass' depth pre-pass fragment shader (grass.cpp): a blade writes depth and no colour, so the colour pass
// behind it - the field's overdraw - shades each pixel once rather than once per blade. Nothing to output.
void main()
{
}
