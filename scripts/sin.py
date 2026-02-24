import math

def write_sine_lut_to_file(filename="../include/sine_lut.h", size=512):
    lut = []
    for i in range(size):
        # Calculate angle (0 to 2pi)
        angle = (i / size) * 2 * math.pi
        
        # Scale and offset to fit 0-255
        # Using 127.5 ensures we utilize the full 8-bit range
        scaled_val = int(round(127.5 + (math.sin(angle) * 127.5)))
        
        # Clamp just in case of rounding floating point errors
        scaled_val = max(0, min(255, scaled_val))
        lut.append(scaled_val)
    
    with open(filename, "w") as f:
        f.write("#ifndef SINE_LUT_H\n#define SINE_LUT_H\n\n")
        f.write("#include <stdint.h>\n\n")
        f.write(f"// Pre-calculated Sine table for {size} steps\n")
        f.write(f"const uint8_t sineTable[{size}] = {{\n")
        
        for i in range(0, size, 12):
            line = lut[i:i+12]
            f.write("    " + ", ".join(f"{v:3}" for v in line) + ",\n")
            
        f.write("};\n\n")
        f.write("#endif // SINE_LUT_H\n")
    
    print(f"File '{filename}' has been generated successfully.")

if __name__ == "__main__":
    write_sine_lut_to_file()