import java.util.*;

import command.*;

public class FreeCommand implements Command
{
	public String getName()
	{
		return "free";
	}
	
	public String getHelp()
	{
		return "Get or set whether the entire chdesc free list is rendered.";
	}
	
	public Object runCommand(String args[], Object data, CommandInterpreter interpreter) throws CommandException
	{
		Debugger dbg = (Debugger) data;
		if(dbg != null)
		{
			String now = "";
			if(args.length != 0)
			{
				if("on".equals(args[0]))
					dbg.setRenderFree(true);
				else if("off".equals(args[0]))
					dbg.setRenderFree(false);
				else
				{
					System.out.println("Invalid setting: " + args[0]);
					return data;
				}
				now = "now ";
			}
			System.out.println("Free list rendering is " + now + (dbg.getRenderFree() ? "on" : "off"));
		}
		else
			System.out.println("Need a file to debug.");
		return data;
	}
}
