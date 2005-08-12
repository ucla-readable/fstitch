/* This file originally comes from the Sun Java Tutorial site. */

import java.awt.*;
import javax.swing.*;

public class ArrowIcon implements Icon, SwingConstants
{
	private int width, height;
	
	private int xPoints[];
	private int yPoints[];
	
	public ArrowIcon(int direction)
	{
		xPoints = new int[4];
		yPoints = new int[4];
		
		switch(direction)
		{
			case WEST:
			case LEFT:
			case LEADING:
				width = 9;
				height = 18;
				xPoints[0] = width;
				yPoints[0] = -1;
				xPoints[1] = width;
				yPoints[1] = height;
				xPoints[2] = 0;
				yPoints[2] = height / 2;
				xPoints[3] = 0;
				yPoints[3] = height / 2 - 1;
				break;
			case EAST:
			case RIGHT:
			case TRAILING:
				width = 9;
				height = 18;
				xPoints[0] = 0;
				yPoints[0] = -1;
				xPoints[1] = 0;
				yPoints[1] = height;
				xPoints[2] = width;
				yPoints[2] = height / 2;
				xPoints[3] = width;
				yPoints[3] = height / 2 - 1;
				break;
			case NORTH:
				width = 18;
				height = 9;
				xPoints[0] = -1;
				yPoints[0] = height;
				xPoints[1] = width;
				yPoints[1] = height;
				xPoints[2] = width / 2;
				yPoints[2] = 0;
				xPoints[3] = width / 2 - 1;
				yPoints[3] = 0;
				break;
			case SOUTH:
				width = 18;
				height = 9;
				xPoints[0] = -1;
				yPoints[0] = 0;
				xPoints[1] = width;
				yPoints[1] = 0;
				xPoints[2] = width / 2;
				yPoints[2] = height;
				xPoints[3] = width / 2 - 1;
				yPoints[3] = height;
				break;
			default:
				throw new RuntimeException("Invalid ArrowIcon direction!");
		}
	}
	
	public int getIconHeight()
	{
		return height;
	}
	
	public int getIconWidth()
	{
		return width;
	}
	
	public void paintIcon(Component c, Graphics g, int x, int y)
	{
		if(c.isEnabled())
			g.setColor(c.getForeground());
		else
			g.setColor(Color.gray);
		
		g.translate(x, y);
		g.fillPolygon(xPoints, yPoints, xPoints.length);
		/* Restore graphics object */
		g.translate(-x, -y);
	}
}
