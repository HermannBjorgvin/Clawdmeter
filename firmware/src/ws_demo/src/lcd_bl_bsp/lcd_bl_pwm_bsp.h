#ifndef LCD_BL_PWM_BSP_H
#define LCD_BL_PWM_BSP_H



#define  LCD_PWM_MODE_0   (0xff-0)
#define  LCD_PWM_MODE_25  (0xff-25)
#define  LCD_PWM_MODE_50  (0xff-50)
#define  LCD_PWM_MODE_75  (0xff-75)
#define  LCD_PWM_MODE_100 (0xff-100)
#define  LCD_PWM_MODE_125 (0xff-125)
#define  LCD_PWM_MODE_150 (0xff-150)
#define  LCD_PWM_MODE_175 (0xff-175)
#define  LCD_PWM_MODE_200 (0xff-200)
#define  LCD_PWM_MODE_225 (0xff-225)
#define  LCD_PWM_MODE_255 (0xff-255)


#ifdef __cplusplus
extern "C" {
#endif

void lcd_bl_pwm_bsp_init(uint16_t duty);
void setUpduty(uint16_t duty);

#ifdef __cplusplus
}
#endif

#endif