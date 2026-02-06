from flask import Blueprint, request, jsonify, current_app, render_template, send_file, Response
import os
from io import BytesIO
from datetime import datetime
from PIL import Image, ImageOps
import numpy as np

main_bp = Blueprint("main", __name__)

# Waveshare 7.3" Spectra6 display configuration
DISPLAY_WIDTH = 800
DISPLAY_HEIGHT = 480
PORTRAIT_MODE = True

# ============================================================
# COLOR PALETTE for Spectra 6 display
# ============================================================
# Index 0 = Black
# Index 1 = White  
# Index 2 = Green
# Index 3 = Blue
# Index 4 = Red
# Index 5 = Yellow
# ============================================================

def _create_palette_image():
    """Create palette image that PIL will strictly adhere to."""
    # Create image with one pixel per color
    palette_img = Image.new('P', (6, 1))
    
    # Define the 6-color palette
    palette_data = [
        0, 0, 0,          # 0 = Black
        255, 255, 255,    # 1 = White
        0, 128, 0,        # 2 = Green
        0, 0, 255,        # 3 = Blue
        255, 0, 0,        # 4 = Red
        255, 255, 0,      # 5 = Yellow
    ] + [0] * (768 - 18)  # Pad to 256 colors
    
    palette_img.putpalette(palette_data)
    
    # Put each color index in a pixel - this forces PIL to use these indices
    pixels = palette_img.load()
    for i in range(6):
        pixels[i, 0] = i
    
    return palette_img

# Pre-create palette image at module load
PALETTE_IMAGE = _create_palette_image()


def resize_and_dither_image(image_path):
    """Resize image to fit display and apply 6-color dithering."""
    img = Image.open(image_path).convert('RGB')

    # Quantize with our strict 6-color palette
    quantized = img.quantize(
        colors=6,
        palette=PALETTE_IMAGE,
        dither=Image.Dither.FLOYDSTEINBERG
    )

    return quantized


def convert_to_display_format(image_path):
    """Convert image to 4bpp packed format using fast PIL quantize."""
    quantized = resize_and_dither_image(image_path)

    # Get pixel indices and pack
    pixels = np.array(quantized, dtype=np.uint8)
    flat = pixels.flatten()
    packed = (flat[0::2] << 4) | flat[1::2]

    return bytes(packed)


@main_bp.route('/')
def main_page():
    device_config = current_app.config['DEVICE_CONFIG']
    return render_template('inky.html', config=device_config.get_config(), plugins=device_config.get_plugins())

@main_bp.route('/api/preview_image')
def preview_image():
    """Preview how the image will look after resize and dithering."""
    image_path = os.path.join(os.path.dirname(os.path.dirname(__file__)), 'static', 'images', 'current_image.png')

    if not os.path.exists(image_path):
        return jsonify({"error": "Image not found"}), 404

    try:
        quantized = resize_and_dither_image(image_path)
        # Convert palette image back to RGB for PNG output
        rgb_image = quantized.convert('RGB')

        # Save to bytes buffer
        buffer = BytesIO()
        rgb_image.save(buffer, format='PNG')
        buffer.seek(0)

        return send_file(buffer, mimetype='image/png')
    except Exception as e:
        return jsonify({"error": str(e)}), 500


@main_bp.route('/api/current_image')
def get_current_image():
    """Serve current_image.png with conditional request support (If-Modified-Since)."""
    image_path = os.path.join(os.path.dirname(os.path.dirname(__file__)), 'static', 'images', 'current_image.png')

    if not os.path.exists(image_path):
        return jsonify({"error": "Image not found"}), 404

    # Get the file's last modified time (truncate to seconds to match HTTP header precision)
    file_mtime = int(os.path.getmtime(image_path))
    last_modified = datetime.fromtimestamp(file_mtime)
    # Check If-Modified-Since header
    if_modified_since = request.headers.get('If-Modified-Since')
    if if_modified_since:
        try:
            # Parse the If-Modified-Since header
            client_mtime = datetime.strptime(if_modified_since, '%a, %d %b %Y %H:%M:%S %Z')
            client_mtime_seconds = int(client_mtime.timestamp())
             # Compare (both now in seconds, no sub-second precision)
            if file_mtime <= client_mtime_seconds:
                return '', 304
        except (ValueError, AttributeError):
            pass

    output_format = request.args.get('format', 'spectra6').lower()

    if output_format in ['raw', 'spectra6']:
        try:
            packed_data = convert_to_display_format(image_path)
            response = Response(packed_data, mimetype='application/octet-stream')
            response.headers['Last-Modified'] = last_modified.strftime('%a, %d %b %Y %H:%M:%S GMT')
            response.headers['Cache-Control'] = 'no-cache'
            response.headers['Content-Length'] = len(packed_data)
            return response
        except Exception as e:
            return jsonify({"error": str(e)}), 500
    else:
        response = send_file(image_path, mimetype='image/png')
        response.headers['Last-Modified'] = last_modified.strftime('%a, %d %b %Y %H:%M:%S GMT')
        response.headers['Cache-Control'] = 'no-cache'
        return response

@main_bp.route('/api/plugin_order', methods=['POST'])
def save_plugin_order():
    """Save the custom plugin order."""
    device_config = current_app.config['DEVICE_CONFIG']

    data = request.get_json() or {}
    order = data.get('order', [])

    if not isinstance(order, list):
        return jsonify({"error": "Order must be a list"}), 400

    device_config.set_plugin_order(order)

    return jsonify({"success": True})