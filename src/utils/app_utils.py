import logging
import os
import socket
import subprocess

from pathlib import Path
from PIL import Image, ImageDraw, ImageFont, ImageOps
from utils.image_utils import pad_image_blur

logger = logging.getLogger(__name__)

FONT_FAMILIES = {
    "Dogica": [{
        "font-weight": "normal",
        "file": "dogicapixel.ttf"
    },{
        "font-weight": "bold",
        "file": "dogicapixelbold.ttf"
    }],
    "Jost": [{
        "font-weight": "normal",
        "file": "Jost.ttf"
    },{
        "font-weight": "bold",
        "file": "Jost-SemiBold.ttf"
    }],
    "Napoli": [{
        "font-weight": "normal",
        "file": "Napoli.ttf"
    }],
    "DS-Digital": [{
        "font-weight": "normal",
        "file": os.path.join("DS-DIGI", "DS-DIGI.TTF")
    }]
}

FONTS = {
    "ds-gigi": "DS-DIGI.TTF",
    "napoli": "Napoli.ttf",
    "jost": "Jost.ttf",
    "jost-semibold": "Jost-SemiBold.ttf"
}

def resolve_path(file_path):
    src_dir = os.getenv("SRC_DIR")
    if src_dir is None:
        # Default to the src directory
        src_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    
    src_path = Path(src_dir)
    return str(src_path / file_path)

def get_ip_address():
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.connect(("8.8.8.8", 80))
        ip_address = s.getsockname()[0]
    return ip_address

def get_wifi_name():
    try:
        output = subprocess.check_output(['iwgetid', '-r']).decode('utf-8').strip()
        return output
    except subprocess.CalledProcessError:
        return None

def is_connected():
    """Check if the Raspberry Pi has an internet connection."""
    try:
        # Try to connect to Google's public DNS server
        socket.create_connection(("8.8.8.8", 53), timeout=2)
        return True
    except OSError:
        return False

def get_font(font_name, font_size=50, font_weight="normal"):
    if font_name in FONT_FAMILIES:
        font_variants = FONT_FAMILIES[font_name]

        font_entry = next((entry for entry in font_variants if entry["font-weight"] == font_weight), None)
        if font_entry is None:
            font_entry = font_variants[0]  # Default to first available variant

        if font_entry:
            font_path = resolve_path(os.path.join("static", "fonts", font_entry["file"]))
            return ImageFont.truetype(font_path, font_size)
        else:
            logger.warning(f"Requested font weight not found: font_name={font_name}, font_weight={font_weight}")
    else:
        logger.warning(f"Requested font not found: font_name={font_name}")

    return None

def get_fonts():
    fonts_list = []
    for font_family, variants in FONT_FAMILIES.items():
        for variant in variants:
            fonts_list.append({
                "font_family": font_family,
                "url": resolve_path(os.path.join("static", "fonts", variant["file"])),
                "font_weight": variant.get("font-weight", "normal"),
                "font_style": variant.get("font-style", "normal"),
            })
    return fonts_list

def get_font_path(font_name):
    return resolve_path(os.path.join("static", "fonts", FONTS[font_name]))

def generate_startup_image(dimensions=(800,480)):
    bg_color = (255,255,255)
    text_color = (0,0,0)
    width, height = dimensions

    hostname = socket.gethostname()
    ip = get_ip_address()

    image = Image.new("RGBA", dimensions, bg_color)
    image_draw = ImageDraw.Draw(image)

    title_font_size = width * 0.145
    image_draw.text((width/2, height/2), "inkypi", anchor="mm", fill=text_color, font=get_font("Jost", title_font_size))

    text = f"To get started, visit http://{hostname}.local"
    text_font_size = width * 0.032

    # Draw the instructions
    y_text = height * 3 / 4
    image_draw.text((width/2, y_text), text, anchor="mm", fill=text_color, font=get_font("Jost", text_font_size))

    # Draw the IP on a line below
    ip_text = f"or http://{ip}"
    ip_text_font_size = width * 0.032
    bbox = image_draw.textbbox((0, 0), text, font=get_font("Jost", text_font_size))
    text_height = bbox[3] - bbox[1]
    ip_y = y_text + text_height * 1.35
    image_draw.text((width/2, ip_y), ip_text, anchor="mm", fill=text_color, font=get_font("Jost", ip_text_font_size))

    return image

def parse_form(request_form):
    request_dict = request_form.to_dict()
    for key in request_form.keys():
        if key.endswith('[]'):
            request_dict[key] = request_form.getlist(key)
    return request_dict

def handle_request_files(request_files, form_data={}, device_config=None):
    allowed_file_extensions = {'pdf', 'png', 'jpg', 'jpeg', 'gif', 'webp', 'heif', 'heic'}
    image_extensions = {'png', 'jpg', 'jpeg', 'gif', 'webp', 'heif', 'heic'}
    file_location_map = {}
    # handle existing file locations being provided as part of the form data
    for key in set(request_files.keys()):
        is_list = key.endswith('[]')
        if key in form_data:
            file_location_map[key] = form_data.getlist(key) if is_list else form_data.get(key)
    # add new files in the request
    for key, file in request_files.items(multi=True):
        is_list = key.endswith('[]')
        file_name = file.filename
        if not file_name:
            continue

        extension = os.path.splitext(file_name)[1].replace('.', '')
        if not extension or extension.lower() not in allowed_file_extensions:
            continue

        file_name = os.path.basename(file_name)

        file_save_dir = resolve_path(os.path.join("static", "images", "saved"))
        file_path = os.path.join(file_save_dir, file_name)

        # Open the image and process it
        if extension.lower() in image_extensions:
            try:
                with Image.open(file) as img:
                    # Apply EXIF transformation
                    img = ImageOps.exif_transpose(img)

                    # Apply resize and rotation if device_config is provided
                    if device_config:
                        border_percent = int(form_data.get("borderPercent", 0))
                        img = _resize_and_rotate_image(img, device_config, border_percent)

                    img.save(file_path)
            except Exception as e:
                logger.warning(f"Image processing error for {file_name}: {e}")
                file.seek(0)
                file.save(file_path)
        else:
            # Directly save non-image files (e.g., PDF)
            file.save(file_path)

        if is_list:
            file_location_map.setdefault(key, [])
            file_location_map[key].append(file_path)
        else:
            file_location_map[key] = file_path
    return file_location_map


def _resize_and_rotate_image(image, device_config, border_percent=0):
    # Resize and rotate image based on device configuration.
    orientation = device_config.get_config("orientation")
    display_size = device_config.get_resolution()

    if orientation == "vertical":
        target_size = (display_size[1], display_size[0])
        # For portrait mode: resize then rotate for display
        if image.width > image.height:
            # Landscape image in portrait mode: pad with blurred background
            fitted_img = pad_image_blur(image, target_size)
            image = fitted_img.rotate(90, expand=True)
        else:
            fitted_img = ImageOps.fit(image, target_size, Image.Resampling.LANCZOS)
            image = fitted_img.rotate(90, expand=True)
    else:
        # Landscape mode: just resize to display dimensions
        image = ImageOps.fit(image, display_size, Image.Resampling.LANCZOS)

    # Apply border if border_percent > 0
    if border_percent > 0:
        canvas_size = image.size
        shrink_factor = 1 - (border_percent / 100)
        new_width = int(canvas_size[0] * shrink_factor)
        new_height = int(canvas_size[1] * shrink_factor)
        shrunk_image = image.resize((new_width, new_height), Image.Resampling.LANCZOS)

        # Create white canvas and paste shrunk image centered
        white_canvas = Image.new("RGB", canvas_size, (255, 255, 255))
        paste_x = (canvas_size[0] - new_width) // 2
        paste_y = (canvas_size[1] - new_height) // 2
        white_canvas.paste(shrunk_image, (paste_x, paste_y))
        image = white_canvas

    return image