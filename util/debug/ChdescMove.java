import java.io.DataInput;
//import java.io.IOException;

public class ChdescMove extends Opcode
{
	private final int chdesc, destination, target;
	private final short offset;
	
	public ChdescMove(int chdesc, int destination, int target, short offset)
	{
		this.chdesc = chdesc;
		this.destination = destination;
		this.target = target;
		this.offset = offset;
	}
	
	public void applyTo(SystemState state)
	{
	}
	
	public boolean hasEffect()
	{
		return false;
	}
	
	public String toString()
	{
		return "KDB_CHDESC_MOVE: chdesc = " + SystemState.hex(chdesc) + ", destination = " + SystemState.hex(destination) + ", target = " + SystemState.hex(target) + ", offset = " + offset;
	}
	
	public static ModuleOpcodeFactory getFactory(DataInput input)
	{
		ModuleOpcodeFactory factory = new ModuleOpcodeFactory(input, KDB_CHDESC_MOVE, "KDB_CHDESC_MOVE", ChdescMove.class);
		factory.addParameter("chdesc", 4);
		factory.addParameter("destination", 4);
		factory.addParameter("target", 4);
		factory.addParameter("offset", 2);
		return factory;
	}
}
