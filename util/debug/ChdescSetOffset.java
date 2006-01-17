import java.io.DataInput;
import java.io.IOException;

public class ChdescSetOffset extends Opcode
{
	private final int chdesc;
	private final short offset;
	
	public ChdescSetOffset(int chdesc, short offset)
	{
		this.chdesc = chdesc;
		this.offset = offset;
	}
	
	public void applyTo(SystemState state)
	{
		Chdesc chdesc = state.lookupChdesc(this.chdesc);
		if(chdesc != null)
			chdesc.setOffset(offset);
	}
	
	public String toString()
	{
		return "KDB_CHDESC_SET_OFFSET: chdesc = " + SystemState.hex(chdesc) + ", offset = " + SystemState.hex(offset);
	}
	
	public static ModuleOpcodeFactory getFactory(DataInput input)
	{
		ModuleOpcodeFactory factory = new ModuleOpcodeFactory(input, KDB_CHDESC_SET_OFFSET, "KDB_CHDESC_SET_OFFSET", ChdescSetOffset.class);
		factory.addParameter("chdesc", 4);
		factory.addParameter("offset", 2);
		return factory;
	}
}
