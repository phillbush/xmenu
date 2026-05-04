struct hsy {
	double hue;
	double saturation;
	double luma;
};

struct rgb {
	double red;
	double green;
	double blue;
};

struct hsy to_hsy(struct rgb rgb);
struct rgb to_rgb(struct hsy hsy);
