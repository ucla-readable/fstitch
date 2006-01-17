import java.io.DataInput;
import java.io.IOException;

public class ChdescRollback extends Opcode
{
	private final int chdesc;
	
	public ChdescRollback(int chdesc)
	{
		this.chdesc = chdesc;
	}
	
	public void applyTo(SystemState state)
	{
		Chdesc chdesc = state.lookupChdesc(this.chdesc);
		if(chdesc != null)
			chdesc.setFlags(Chdesc.FLAG_ROLLBACK);
	}
	
	public String toString()
	{
		return "KDB_CHDESC_ROLLBACK: chdesc = " + SystemState.hex(chdesc);
	}
	
	public static ModuleOpcodeFactory getFactory(DataInput input)
	{
		ModuleOpcodeFactory factory = new ModuleOpcodeFactory(input, KDB_CHDESC_ROLLBACK, "KDB_CHDESC_ROLLBACK", ChdescRollback.class);
		factory.addParameter("chdesc", 4);
		return factory;
	}
}
