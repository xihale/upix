# CrossScale Models

This directory contains pre-trained super-resolution models.

## Model Format

CrossScale uses ncnn format models with the following files:
- `.param` - Model architecture definition
- `.bin` - Model weights

## Supported Models

You can use any ncnn-trained super-resolution model, including:
- Real-ESRGAN
- RealSR
- ESRGAN
- SRResNet
- Custom models

## Adding Models

1. Copy your `.param` and `.bin` files to this directory
2. Ensure both files have matching names (e.g., `model.param` and `model.bin`)
3. CrossScale will automatically detect and list available models

## Model Metadata

CrossScale automatically infers:
- Scale factor (2x, 4x, etc.)
- Input channels (RGB or Grayscale)
- Recommended tile size
