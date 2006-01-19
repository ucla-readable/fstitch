import java.util.*;

import command.*;

public class OptionCommand implements Command
{
	public String getName()
	{
		return "option";
	}
	
	public String getHelp()
	{
		return "Get or set rendering options: freelist, grouping.";
	}
	
	public Object runCommand(String args[], Object data, CommandInterpreter interpreter) throws CommandException
	{
		Debugger dbg = (Debugger) data;
		if(dbg != null)
		{
			if(args.length > 0)
			{
				if("freelist".equals(args[0]))
				{
					String now = "";
					if(args.length != 1)
					{
						if("on".equals(args[1]))
							dbg.setRenderFree(true);
						else if("off".equals(args[1]))
							dbg.setRenderFree(false);
						else
						{
							System.out.println("Invalid setting: " + args[1]);
							return data;
						}
						now = "now ";
					}
					System.out.println("Free list rendering is " + now + (dbg.getRenderFree() ? "on" : "off"));
				}
				else if("grouping".equals(args[0]))
				{
					String now = "";
					if(args.length != 1)
					{
						if("off".equals(args[1]) || "block".equals(args[1]) || "owner".equals(args[1]))
							dbg.setGroupingType(args[1]);
						else if("block-owner".equals(args[1]) || "owner-block".equals(args[1]))
							dbg.setGroupingType(args[1]);
						else
						{
							System.out.println("Invalid setting: " + args[1]);
							return data;
						}
						now = "now ";
					}
					System.out.println("Change descriptor grouping is " + now + dbg.getGroupingType());
				}
				else
					System.out.println("Invalid option: " + args[0]);
			}
			else
				System.out.println("Need an option to get or set.");
		}
		else
			System.out.println("Need a file to debug.");
		return data;
	}
}
